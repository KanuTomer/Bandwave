#include <windows.h>
#include <unknwn.h>
#include <appmodel.h>
#include "controller.hpp"
#include "widget_card.hpp"
#include "widget_images.hpp"
#include "native_editor.hpp"
#include "library.hpp"
#include "ipc.hpp"
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Microsoft.Windows.Widgets.Providers.h>
#include <winrt/Microsoft.Windows.Widgets.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <sddl.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <ctime>
#include <dbt.h>
#include <bthdef.h>

namespace wp=winrt::Microsoft::Windows::Widgets::Providers;
using winrt::Microsoft::Windows::Widgets::WidgetSize;
using namespace winrt::Windows::Data::Json;
constexpr GUID ProviderClsid{0x891e4d36,0xa7cb,0x423c,{0xb1,0x50,0x96,0xd9,0xeb,0x10,0x71,0xc7}};
constexpr UINT AppDispatchMessage=WM_APP+1, StateMessage=WM_APP+2;

// Shared with the app and kept outside the package (see library.hpp), so EQs and settings survive
// updates and only leave when the uninstaller is told to remove them. 0.4.x state moves across once.
static std::filesystem::path data_folder() {
    auto folder=earbud::data_folder();
    PWSTR raw{};winrt::check_hresult(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&raw));
    std::filesystem::path local(raw);CoTaskMemFree(raw);
    UINT32 length{};GetCurrentPackageFamilyName(&length,nullptr);
    if(length) { std::wstring family(length,L'\0');if(GetCurrentPackageFamilyName(&length,family.data())==ERROR_SUCCESS){family.resize(length-1);earbud::migrate_package_state(folder,local/L"Packages"/family/L"LocalState");} }
    return folder;
}
static std::string read_file(const std::filesystem::path& path) {
    std::error_code ec;
    if(!std::filesystem::exists(path,ec) || std::filesystem::file_size(path,ec)>65536) return {};
    std::ifstream input(path,std::ios::binary);return {std::istreambuf_iterator<char>(input),{}};
}
static Snapshot load_cache(const std::filesystem::path& path,bool& logging) {
    Snapshot s;
    try {
        auto text=read_file(path);if(text.empty())return s;
        auto json=JsonObject::Parse(winrt::to_hstring(text));
        logging=json.GetNamedBoolean(L"logging",false);
        s.release_when_hidden=json.GetNamedBoolean(L"release_when_hidden",false);
        s.address=winrt::to_string(json.GetNamedString(L"address",L""));
        s.firmware=winrt::to_string(json.GetNamedString(L"firmware",L""));
        s.checked=static_cast<int64_t>(json.GetNamedNumber(L"checked",0));
        s.noise=static_cast<int>(json.GetNamedNumber(L"noise",-1));
        s.ambient=static_cast<int>(json.GetNamedNumber(L"ambient",-1));s.voice=static_cast<int>(json.GetNamedNumber(L"voice",-1));
        s.noise_supported=json.GetNamedBoolean(L"noise_supported",false);
        s.ambient_supported=json.GetNamedBoolean(L"ambient_supported",false);
        s.eq_supported=json.GetNamedBoolean(L"eq_supported",false);
        s.preset=static_cast<uint32_t>(json.GetNamedNumber(L"preset",0));s.bass=static_cast<int>(json.GetNamedNumber(L"bass",0));
        if(json.HasKey(L"bands")){auto bands=json.GetNamedArray(L"bands");if(bands.Size()==5)for(auto value:bands)s.bands.push_back(static_cast<int32_t>(value.GetNumber()));}
        if(json.HasKey(L"presets")){auto presets=json.GetNamedArray(L"presets");if(presets.Size()<=64)for(auto value:presets){auto preset=value.GetObject();s.presets.emplace_back(static_cast<uint32_t>(preset.GetNamedNumber(L"id")),winrt::to_string(preset.GetNamedString(L"name")));}}
        s.batteries={static_cast<int>(json.GetNamedNumber(L"left",-1)),static_cast<int>(json.GetNamedNumber(L"right",-1)),static_cast<int>(json.GetNamedNumber(L"case",-1))};
        s.status=s.checked?"Controls idle":"Not connected";
    }catch(...){}
    return s;
}
static std::string icon_data(const std::filesystem::path& path){
    auto bytes=read_file(path);if(bytes.empty())return {};
    return png_uri({reinterpret_cast<const uint8_t*>(bytes.data()),bytes.size()});
}
struct WidgetRecord { WidgetSize size=WidgetSize::Medium;bool active{};std::string last_template; };
class App;
struct WidgetProvider : winrt::implements<WidgetProvider,wp::IWidgetProvider,wp::IWidgetProvider2> {
    App& app;
    explicit WidgetProvider(App& owner);
    ~WidgetProvider();
    void CreateWidget(wp::WidgetContext context);
    void DeleteWidget(winrt::hstring const& id,winrt::hstring const&);
    void OnActionInvoked(wp::WidgetActionInvokedArgs args);
    void OnWidgetContextChanged(wp::WidgetContextChangedArgs args);
    void OnCustomizationRequested(wp::WidgetCustomizationRequestedArgs args);
    void Activate(wp::WidgetContext context);
    void Deactivate(winrt::hstring id);
};

class App {
public:
    HWND dispatcher{},editor{};
    std::unique_ptr<Controller> controller;
    std::atomic<int> references{};
    std::map<std::wstring,WidgetRecord> widgets;
    WidgetIcons icons;
    std::filesystem::path assets_path;
    std::string battery_key;
    bool artwork_loaded{},artwork_contrast{};
    COLORREF artwork_text{};
    HPOWERNOTIFY power_notification{};
    HDEVNOTIFY interface_notification{};
    std::vector<std::pair<HANDLE,HDEVNOTIFY>> radio_notifications;
    bool logging{}, provider_started{}, editor_started{};
    std::filesystem::path folder;
    std::mutex dispatch_mutex;
    std::deque<std::function<void()>> tasks;
    HANDLE stop_pipe=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    std::thread pipe_thread;
    std::wstring pipe_name;
    std::unique_ptr<NativeEditor> native;
    // The app (EarbudControls.UI.exe) talks to this process over a second pipe (see ipc.hpp).
    // This process stays the only Bluetooth owner and the only writer of library.json.
    earbud::Library library;
    bool app_active{},import_requested{};
    std::atomic<bool> app_connected{};
    std::thread link_thread;
    std::mutex link_mutex;
    std::deque<std::string> outbox;
    HANDLE outbox_event=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    App():folder(data_folder()){}
    ~App(){if(power_notification)UnregisterSuspendResumeNotification(power_notification);if(interface_notification)UnregisterDeviceNotification(interface_notification);for(auto [radio,notice]:radio_notifications){if(notice)UnregisterDeviceNotification(notice);CloseHandle(radio);}SetEvent(stop_pipe);if(pipe_thread.joinable())pipe_thread.join();if(link_thread.joinable())link_thread.join();CloseHandle(outbox_event);native.reset();controller.reset();if(dispatcher)DestroyWindow(dispatcher);CloseHandle(stop_pipe);}
    void post(std::function<void()> fn){ {std::lock_guard lock(dispatch_mutex);tasks.push_back(std::move(fn));}PostMessageW(dispatcher,AppDispatchMessage,0,0); }
    void dispatch(){std::deque<std::function<void()>> work;{std::lock_guard lock(dispatch_mutex);work.swap(tasks);}for(auto& fn:work){try{fn();}catch(const winrt::hresult_error& e){diagnostic(winrt::to_string(e.message()));}catch(const std::exception& e){diagnostic(e.what());}}}
    void diagnostic(const std::string& text){if(!logging)return;try{auto path=folder/L"diagnostics.log";std::error_code ec;bool reset=std::filesystem::exists(path,ec)&&std::filesystem::file_size(path,ec)>5*1024*1024;std::ofstream out(path,reset?std::ios::trunc:std::ios::app);out<<checked_time(std::time(nullptr))<<" "<<text.substr(0,2048)<<"\n";}catch(...){}}
    void init(bool show) {
        wchar_t module[32768]{};auto count=GetModuleFileNameW(nullptr,module,32768);
        if(count&&count<32768)assets_path=std::filesystem::path(module).parent_path()/L"Public"/L"Icons";
        WNDCLASSW wc{};wc.lpfnWndProc=window_proc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"EarbudControls.Dispatcher";
        RegisterClassW(&wc);dispatcher=CreateWindowExW(0,wc.lpszClassName,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,wc.hInstance,this);
        if(!dispatcher)winrt::throw_last_error();
        auto initial=load_cache(folder/L"state.json",logging);
        library=earbud::load_library(folder);
        std::filesystem::create_directories(folder);
        controller=std::make_unique<Controller>([this]{PostMessageW(dispatcher,StateMessage,0,0);},std::move(initial));
        power_notification=RegisterSuspendResumeNotification(dispatcher,DEVICE_NOTIFY_WINDOW_HANDLE);
        DEV_BROADCAST_DEVICEINTERFACE_W filter{};filter.dbcc_size=sizeof(filter);filter.dbcc_devicetype=DBT_DEVTYP_DEVICEINTERFACE;filter.dbcc_classguid=GUID_BTHPORT_DEVICE_INTERFACE;
        interface_notification=RegisterDeviceNotificationW(dispatcher,&filter,DEVICE_NOTIFY_WINDOW_HANDLE);
        watch_radios();
        if(show)open(0);
        SetTimer(dispatcher,1,30000,nullptr);
    }
    void interest(){bool active=editor!=nullptr||app_active;for(auto& [id,widget]:widgets)active|=widget.active;controller->interest(active);}
    void recover(){
        try{for(auto info:wp::WidgetManager::GetDefault().GetWidgetInfos()){auto context=info.WidgetContext();widgets[std::wstring(context.Id())].size=context.Size();}update_widgets(true);}
        catch(const winrt::hresult_error& e){diagnostic(winrt::to_string(e.message()));}
    }
    void artwork(const Snapshot& state){
        HIGHCONTRASTW contrast{sizeof(contrast)};SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(contrast),&contrast,0);
        bool high=(contrast.dwFlags&HCF_HIGHCONTRASTON)!=0;auto foreground=GetSysColor(COLOR_WINDOWTEXT);
        if(!artwork_loaded||artwork_contrast!=high||artwork_text!=foreground){
            for(auto name:{"case","settings","noise-cancelling","ambient","off","equalizer","back","more","previous","next","plus","minus","check","bluetooth","bass","link","diagnostics","log"}){
                bool white=GetRValue(foreground)+GetGValue(foreground)+GetBValue(foreground)>384;
                std::string light=std::string(name)+(high?(white?"-hc-dark.png":"-hc-light.png"):"-light.png"),dark=std::string(name)+(high?(white?"-hc-dark.png":"-hc-light.png"):"-dark.png");
                icons.symbols[name]={icon_data(assets_path/light),icon_data(assets_path/dark)};
                icons.symbols[std::string(name)+"-selected"]=high?icons.symbols[name]:std::pair{icon_data(assets_path/(std::string(name)+"-accent-light.png")),icon_data(assets_path/(std::string(name)+"-accent.png"))};
            }
            icons.light=icons.symbols["equalizer"].first;icons.dark=icons.symbols["equalizer"].second;
            icons.marker={selection_marker(false,high),selection_marker(true,high)};
        icons.border={border_image(false,high),border_image(true,high)};icons.surface={button_surface(false,high),button_surface(true,high)};
            artwork_loaded=true;artwork_contrast=high;artwork_text=foreground;battery_key.clear();
        }
        auto batteries=std::to_string(state.batteries[0])+":"+std::to_string(state.batteries[1])+":"+std::to_string(state.batteries[2]);
        if(batteries!=battery_key){for(int i=0;i<3;++i)icons.batteries[i]={battery_image(state.batteries[i],i==2,false,high),battery_image(state.batteries[i],i==2,true,high)};battery_key=std::move(batteries);}
    }
    // The app's six main-screen EQs. One is marked when the earbuds are playing it: its Sony
    // preset is selected, or Manual holds exactly its curve.
    std::vector<WidgetEq> widget_eqs(const Snapshot& s){
        std::vector<WidgetEq> eqs;
        for(auto id:library.main_screen()){
            auto const* p=library.find(id);if(!p)continue;
            bool selected=false;
            if(s.fresh&&s.eq_supported){
                if(p->kind==earbud::ProfileKind::Sony)selected=s.preset==p->preset;
                else if(s.preset==MDR_EQ_CUSTOM&&s.bands.size()==5){selected=s.bass==p->clear_bass;for(size_t b=0;b<5;++b)selected=selected&&s.bands[b]==p->bands[b];}
            }
            eqs.push_back({p->id,winrt::to_string(p->name),selected});
        }
        return eqs;
    }
    void update_widgets(bool force=false){auto s=controller->snapshot();artwork(s);auto eqs=widget_eqs(s);for(auto& [id,record]:widgets){if(!force&&!record.active)continue;try{auto card=make_card(s,record.size,eqs,icons);if(!force&&card==record.last_template)continue;wp::WidgetUpdateRequestOptions update(id);update.Template(winrt::to_hstring(card));update.Data(L"{}");wp::WidgetManager::GetDefault().UpdateWidget(update);record.last_template=std::move(card);}catch(const winrt::hresult_error& e){diagnostic(winrt::to_string(e.message()));}}}
    void changed(){
        auto state=controller->snapshot();sync_library(state);update_widgets();refresh_editor();send_snapshot(state);
        if(state.checked&&!state.pending)save(state);
        if(!state.error.empty())diagnostic(state.error);
        maybe_exit();
    }
    void save(const Snapshot& s){
        try{JsonObject j;j.SetNamedValue(L"address",JsonValue::CreateStringValue(winrt::to_hstring(s.address)));j.SetNamedValue(L"firmware",JsonValue::CreateStringValue(winrt::to_hstring(s.firmware)));
            j.SetNamedValue(L"logging",JsonValue::CreateBooleanValue(logging));j.SetNamedValue(L"checked",JsonValue::CreateNumberValue(static_cast<double>(s.checked)));
            j.SetNamedValue(L"release_when_hidden",JsonValue::CreateBooleanValue(s.release_when_hidden));
            j.SetNamedValue(L"noise",JsonValue::CreateNumberValue(s.noise));j.SetNamedValue(L"ambient",JsonValue::CreateNumberValue(s.ambient));j.SetNamedValue(L"voice",JsonValue::CreateNumberValue(s.voice));
            j.SetNamedValue(L"noise_supported",JsonValue::CreateBooleanValue(s.noise_supported));j.SetNamedValue(L"ambient_supported",JsonValue::CreateBooleanValue(s.ambient_supported));j.SetNamedValue(L"eq_supported",JsonValue::CreateBooleanValue(s.eq_supported));
            j.SetNamedValue(L"preset",JsonValue::CreateNumberValue(s.preset));j.SetNamedValue(L"bass",JsonValue::CreateNumberValue(s.bass));
            JsonArray bands;for(auto band:s.bands)bands.Append(JsonValue::CreateNumberValue(band));j.SetNamedValue(L"bands",bands);
            JsonArray presets;for(auto& [id,name]:s.presets){JsonObject preset;preset.SetNamedValue(L"id",JsonValue::CreateNumberValue(id));preset.SetNamedValue(L"name",JsonValue::CreateStringValue(winrt::to_hstring(name)));presets.Append(preset);}j.SetNamedValue(L"presets",presets);
            const wchar_t* keys[]={L"left",L"right",L"case"};for(int i=0;i<3;++i)j.SetNamedValue(keys[i],JsonValue::CreateNumberValue(s.batteries[i]));
            auto temporary=folder/L"state.tmp";{std::ofstream out(temporary,std::ios::binary|std::ios::trunc);out<<winrt::to_string(j.Stringify());if(!out)throw std::runtime_error("Could not save settings.");}
            if(!MoveFileExW(temporary.c_str(),(folder/L"state.json").c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))winrt::throw_last_error();
        }catch(const std::exception& e){diagnostic(e.what());}catch(const winrt::hresult_error& e){diagnostic(winrt::to_string(e.message()));}
    }
    void maybe_exit(){if(!editor && !app_connected && references==0 && (provider_started||editor_started) && !controller->snapshot().ready && !controller->snapshot().pending)PostQuitMessage(0);}
    void action(const std::wstring& id,const std::wstring& verb){
        auto widget=widgets.find(id);if(widget==widgets.end())return;
        if(!widget_action_allowed(widget->second.size,verb))return;
        widget->second.active=true;interest();
        auto state=controller->snapshot();
        if(auto id=suffix_number(verb,L"eq.profile.");id>0){
            auto const* p=library.find(static_cast<uint32_t>(id));if(!p)return; // a removed EQ is never swapped for another
            if(p->kind==earbud::ProfileKind::Sony){if(state.preset!=p->preset)controller->send({Operation::Preset,static_cast<int>(p->preset)});}
            else{Command apply{Operation::ApplyProfile};apply.bands.assign(p->bands.begin(),p->bands.end());apply.bass=p->clear_bass;controller->send(std::move(apply));}
            update_widgets();return;
        }
        if(verb==L"noise.anc"||verb==L"noise.ambient"||verb==L"noise.off")controller->send({Operation::Noise,verb==L"noise.anc"?1:verb==L"noise.ambient"?2:0});
    }
    bool start_in_tray=false; // launched by the startup task at sign-in: the app starts in the tray
    void open(int page){
        editor_started=true;
        // The Dusk app is its own process. If it is already connected, bring it forward.
        if(app_connected){JsonObject show;show.SetNamedValue(L"type",JsonValue::CreateStringValue(L"show"));show.SetNamedValue(L"page",JsonValue::CreateNumberValue(page));send_app(show);return;}
        wchar_t module[32768]{};auto count=GetModuleFileNameW(nullptr,module,32768);
        if(count&&count<32768){
            auto ui=std::filesystem::path(module).parent_path()/L"EarbudControls.UI.exe";
            std::error_code ec;
            if(std::filesystem::exists(ui,ec)){auto args=start_in_tray?std::wstring(L"--tray"):L"--page="+std::to_wstring(page);start_in_tray=false;ShellExecuteW(nullptr,L"open",ui.c_str(),args.c_str(),nullptr,SW_SHOWNORMAL);return;}
        }
        if(native&&native->window){native->show(page);return;}
        native=std::make_unique<NativeEditor>([this]{return controller->snapshot();},[this](Command command){controller->send(std::move(command));},
            [this]{editor=nullptr;interest();post([this]{if(native&&!native->window)native.reset();maybe_exit();});},
            [this]{return logging;},[this](bool enabled){logging=enabled;save(controller->snapshot());});
        editor=native->window;native->show(page);interest();
    }
    void refresh_editor(){if(native)native->refresh();}
    void watch_radios(){
        for(auto [radio,notice]:radio_notifications){if(notice)UnregisterDeviceNotification(notice);CloseHandle(radio);}radio_notifications.clear();
        BLUETOOTH_FIND_RADIO_PARAMS search{sizeof(search)};HANDLE radio{};
        auto found=BluetoothFindFirstRadio(&search,&radio);if(!found)return;
        do{DEV_BROADCAST_HANDLE filter{};filter.dbch_size=sizeof(filter);filter.dbch_devicetype=DBT_DEVTYP_HANDLE;filter.dbch_handle=radio;auto notice=RegisterDeviceNotificationW(dispatcher,&filter,DEVICE_NOTIFY_WINDOW_HANDLE);radio_notifications.emplace_back(radio,notice);}while(BluetoothFindNextRadio(found,&radio));
        BluetoothFindRadioClose(found);
    }
    bool remembered(BTH_ADDR value){
        char address[18]{};snprintf(address,sizeof(address),"%02X:%02X:%02X:%02X:%02X:%02X",static_cast<unsigned>((value>>40)&255),static_cast<unsigned>((value>>32)&255),static_cast<unsigned>((value>>24)&255),static_cast<unsigned>((value>>16)&255),static_cast<unsigned>((value>>8)&255),static_cast<unsigned>(value&255));return controller->snapshot().address==address;
    }
    void device_event(WPARAM event,LPARAM data){
        if(!data)return;auto* header=reinterpret_cast<DEV_BROADCAST_HDR*>(data);
        if(event==DBT_CUSTOMEVENT&&header->dbch_devicetype==DBT_DEVTYP_HANDLE){
            auto* notification=reinterpret_cast<DEV_BROADCAST_HANDLE*>(data);
            if(header->dbch_size<offsetof(DEV_BROADCAST_HANDLE,dbch_data)+sizeof(BTH_HCI_EVENT_INFO))return;
            if(notification->dbch_eventguid==GUID_BLUETOOTH_HCI_EVENT){BTH_HCI_EVENT_INFO info{};std::memcpy(&info,notification->dbch_data,sizeof(info));if(info.connectionType==HCI_CONNECTION_TYPE_ACL&&remembered(info.bthAddress))controller->lifecycle(info.connected?Operation::Reconnect:Operation::Disconnect);}
        }else if(header->dbch_devicetype==DBT_DEVTYP_DEVICEINTERFACE&&(event==DBT_DEVICEARRIVAL||event==DBT_DEVICEREMOVECOMPLETE)){
            if(event==DBT_DEVICEREMOVECOMPLETE)controller->lifecycle(Operation::Disconnect);
            watch_radios();
        }
    }
    void send_app(const JsonObject& message){
        if(!app_connected)return;
        {std::lock_guard lock(link_mutex);if(outbox.size()>64)outbox.pop_front();outbox.push_back(earbud::ipc::frame(winrt::to_string(message.Stringify())));}
        SetEvent(outbox_event);
    }
    void send_snapshot(const Snapshot& s){
        if(!app_connected)return;
        JsonObject j;
        auto number=[&](const wchar_t* key,double value){j.SetNamedValue(key,JsonValue::CreateNumberValue(value));};
        auto flag=[&](const wchar_t* key,bool value){j.SetNamedValue(key,JsonValue::CreateBooleanValue(value));};
        j.SetNamedValue(L"type",JsonValue::CreateStringValue(L"snapshot"));
        flag(L"ready",s.ready);flag(L"pending",s.pending);flag(L"fresh",s.fresh);
        flag(L"noise_supported",s.noise_supported);flag(L"ambient_supported",s.ambient_supported);flag(L"eq_supported",s.eq_supported);
        j.SetNamedValue(L"status",JsonValue::CreateStringValue(winrt::to_hstring(s.status)));
        j.SetNamedValue(L"error",JsonValue::CreateStringValue(winrt::to_hstring(s.error)));
        j.SetNamedValue(L"firmware",JsonValue::CreateStringValue(winrt::to_hstring(s.firmware)));
        number(L"generation",s.connection_generation);number(L"checked",static_cast<double>(s.checked));
        number(L"left",s.batteries[0]);number(L"right",s.batteries[1]);number(L"case",s.batteries[2]);
        number(L"noise",s.noise);number(L"ambient",s.ambient);number(L"voice",s.voice);number(L"preset",s.preset);number(L"bass",s.bass);
        JsonArray bands;for(auto band:s.bands)bands.Append(JsonValue::CreateNumberValue(band));j.SetNamedValue(L"bands",bands);
        JsonArray presets;for(auto& [id,name]:s.presets){JsonObject p;p.SetNamedValue(L"id",JsonValue::CreateNumberValue(id));p.SetNamedValue(L"name",JsonValue::CreateStringValue(winrt::to_hstring(name)));presets.Append(p);}j.SetNamedValue(L"presets",presets);
        j.SetNamedValue(L"library",earbud::to_json(library));
        JsonObject settings;for(auto& [key,value]:s.settings)settings.SetNamedValue(winrt::to_hstring(key),JsonValue::CreateNumberValue(value));j.SetNamedValue(L"settings",settings);
        JsonObject options;for(auto& [key,list]:s.setting_options){JsonArray a;for(int v:list)a.Append(JsonValue::CreateNumberValue(v));options.SetNamedValue(winrt::to_hstring(key),a);}j.SetNamedValue(L"setting_options",options);
        JsonObject names;for(auto& [key,name]:s.setting_names)names.SetNamedValue(winrt::to_hstring(key),JsonValue::CreateStringValue(winrt::to_hstring(name)));j.SetNamedValue(L"setting_names",names);
        JsonArray devices;for(auto& name:s.devices)devices.Append(JsonValue::CreateStringValue(winrt::to_hstring(name)));j.SetNamedValue(L"devices",devices);
        flag(L"release_when_hidden",s.release_when_hidden);
        send_app(j);
    }
    // The library follows the earbuds: Sony presets come from the device with their real curves,
    // and on the first connection Custom 1 and Custom 2 are imported and kept in their slots.
    void sync_library(const Snapshot& s){
        if(!s.ready||!s.eq_supported||s.presets.empty())return;
        bool dirty=false;
        auto sony=[](uint32_t id){return id!=MDR_EQ_CUSTOM&&id!=MDR_EQ_USER_1&&id!=MDR_EQ_USER_2;};
        if(library.profiles.empty()){
            for(auto& [id,name]:s.presets)if(sony(id)){earbud::Profile p;p.id=library.next_id++;p.name=winrt::to_hstring(name).c_str();p.kind=earbud::ProfileKind::Sony;p.preset=id;p.curve_known=false;library.profiles.push_back(std::move(p));}
            dirty=true;
        }
        for(auto& p:library.profiles){
            if(p.kind!=earbud::ProfileKind::Sony)continue;
            auto curve=s.curves.find(p.preset);if(curve==s.curves.end()||curve->second.first.size()!=5)continue;
            std::array<int,5> bands{};for(size_t b=0;b<5;++b)bands[b]=curve->second.first[b];
            if(!p.curve_known||p.bands!=bands||p.clear_bass!=curve->second.second){p.bands=bands;p.clear_bass=curve->second.second;p.curve_known=true;dirty=true;}
        }
        if(!library.imported&&s.fresh&&!s.pending){
            auto has=[&](uint32_t id){return std::any_of(s.presets.begin(),s.presets.end(),[&](auto& p){return p.first==id;});};
            const bool known=(!has(MDR_EQ_USER_1)||s.curves.count(MDR_EQ_USER_1))&&(!has(MDR_EQ_USER_2)||s.curves.count(MDR_EQ_USER_2));
            if(known){
                size_t at=std::min<size_t>(1,library.profiles.size());
                for(auto [slot,keep]:{std::pair{MDR_EQ_USER_1,0},std::pair{MDR_EQ_USER_2,1}}){
                    auto curve=s.curves.find(slot);if(curve==s.curves.end()||curve->second.first.size()!=5)continue;
                    earbud::Profile p;p.id=library.next_id++;p.name=keep?L"Custom 2 (earbuds)":L"Custom 1 (earbuds)";
                    for(size_t b=0;b<5;++b)p.bands[b]=std::clamp<int>(curve->second.first[b],-10,10);
                    p.clear_bass=std::clamp(curve->second.second,-10,10);
                    library.kept[static_cast<size_t>(keep)]=p.id;
                    library.profiles.insert(library.profiles.begin()+static_cast<std::ptrdiff_t>(at++),std::move(p));
                }
                library.imported=true;dirty=true;
            }else if(!import_requested&&controller->send({Operation::ReadSlots})){import_requested=true;}
        }
        if(dirty&&!earbud::save_library(folder,library))diagnostic("Could not save the EQ library.");
    }
    void app_message(const std::string& text){
        JsonObject message{nullptr};
        if(!JsonObject::TryParse(winrt::to_hstring(text),message))return;
        const std::wstring type{message.GetNamedString(L"type",L"")};
        if(type==L"hello"){send_snapshot(controller->snapshot());return;}
        if(type==L"interest"){app_active=message.GetNamedBoolean(L"active",false);interest();return;}
        if(type==L"library"){
            if(!message.HasKey(L"library")||message.GetNamedValue(L"library").ValueType()!=JsonValueType::Object)return;
            auto next=earbud::from_json(std::wstring(message.GetNamedObject(L"library").Stringify()));
            if(!next)return; // the owner never saves a library it can't validate
            library=*next;if(!earbud::save_library(folder,library))diagnostic("Could not save the EQ library.");
            update_widgets();return;
        }
        if(type!=L"command")return;
        const double id=message.GetNamedNumber(L"id",0);
        auto state=controller->snapshot();
        JsonObject result;result.SetNamedValue(L"type",JsonValue::CreateStringValue(L"result"));result.SetNamedValue(L"id",JsonValue::CreateNumberValue(id));
        auto reply=[&](bool accepted){result.SetNamedValue(L"accepted",JsonValue::CreateBooleanValue(accepted));send_app(result);};
        const std::wstring op{message.GetNamedString(L"op",L"")};
        // A command built against an earlier connection is refused, never replayed.
        if(op!=L"retry"&&static_cast<uint32_t>(message.GetNamedNumber(L"generation",-1))!=state.connection_generation){reply(false);return;}
        const int value=static_cast<int>(message.GetNamedNumber(L"value",0));
        auto curve=[&](Command& command){
            if(!message.HasKey(L"bands"))return false;auto bands=message.GetNamedArray(L"bands");if(bands.Size()!=5)return false;
            for(uint32_t b=0;b<5;++b){double v=bands.GetNumberAt(b);if(v<-10||v>10)return false;command.bands.push_back(static_cast<int32_t>(v));}
            double bass=message.GetNamedNumber(L"bass",99);if(bass<-10||bass>10)return false;command.bass=static_cast<int>(bass);return true;
        };
        Command command{Operation::Retry};
        if(op==L"noise"&&value>=0&&value<=2)command={Operation::Noise,value};
        else if(op==L"ambient"&&value>=1&&value<=20)command={Operation::Ambient,value};
        else if(op==L"voice"&&(value==0||value==1))command={Operation::Voice,value};
        else if(op==L"preset"&&value>=0&&value<=255)command={Operation::Preset,value};
        else if(op==L"apply"){command={Operation::ApplyProfile};if(!curve(command)){reply(false);return;}}
        else if(op==L"keep"&&(value==1||value==2)){command={Operation::KeepSlot,value==1?static_cast<int>(MDR_EQ_USER_1):static_cast<int>(MDR_EQ_USER_2)};if(!curve(command)){reply(false);return;}}
        else if(op==L"setting"){command={Operation::Setting,value};command.key=winrt::to_string(message.GetNamedString(L"key",L""));}
        else if(op==L"power_off")command={Operation::PowerOff};
        else if(op==L"release_when_hidden"&&(value==0||value==1)){controller->release_when_hidden(value==1);reply(true);return;}
        else if(op!=L"retry"){reply(false);return;}
        reply(controller->send(std::move(command)));
    }
    void start_link(PSECURITY_DESCRIPTOR descriptor){
        link_thread=std::thread([this,descriptor]{
            SECURITY_ATTRIBUTES attributes{sizeof(attributes),descriptor,FALSE};
            const auto name=earbud::ipc::app_pipe(pipe_name);
            for(;;){
                HANDLE pipe=CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,64*1024,64*1024,0,&attributes);
                if(pipe==INVALID_HANDLE_VALUE)return;
                HANDLE read_event=CreateEventW(nullptr,TRUE,FALSE,nullptr);OVERLAPPED ov{};ov.hEvent=read_event;
                BOOL connected=ConnectNamedPipe(pipe,&ov);DWORD error=connected?ERROR_SUCCESS:GetLastError();
                bool ready=connected||error==ERROR_PIPE_CONNECTED;
                if(error==ERROR_IO_PENDING){HANDLE waits[]={stop_pipe,read_event};ready=WaitForMultipleObjects(2,waits,FALSE,INFINITE)==WAIT_OBJECT_0+1;}
                if(ready){
                    {std::lock_guard lock(link_mutex);outbox.clear();}ResetEvent(outbox_event);
                    app_connected=true;
                    earbud::ipc::Reader reader;char buffer[4096];bool reading=false,alive=true;
                    while(alive){
                        if(!reading){ResetEvent(read_event);ov={};ov.hEvent=read_event;DWORD got{};
                            if(ReadFile(pipe,buffer,sizeof(buffer),&got,&ov)){reader.feed({buffer,got});while(auto m=reader.next())post([this,m=*m]{app_message(m);});if(reader.failed())break;continue;}
                            if(GetLastError()!=ERROR_IO_PENDING)break;reading=true;}
                        HANDLE waits[]={stop_pipe,read_event,outbox_event};
                        auto waited=WaitForMultipleObjects(3,waits,FALSE,INFINITE);
                        if(waited==WAIT_OBJECT_0+1){DWORD got{};reading=false;if(!GetOverlappedResult(pipe,&ov,&got,FALSE))break;reader.feed({buffer,got});while(auto m=reader.next())post([this,m=*m]{app_message(m);});if(reader.failed())break;}
                        else if(waited==WAIT_OBJECT_0+2){
                            std::deque<std::string> pending;{std::lock_guard lock(link_mutex);pending.swap(outbox);ResetEvent(outbox_event);}
                            for(auto& bytes:pending){OVERLAPPED wov{};wov.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);DWORD put{};
                                BOOL ok=WriteFile(pipe,bytes.data(),static_cast<DWORD>(bytes.size()),&put,&wov);
                                if(!ok&&GetLastError()==ERROR_IO_PENDING)ok=WaitForSingleObject(wov.hEvent,5000)==WAIT_OBJECT_0&&GetOverlappedResult(pipe,&wov,&put,FALSE);
                                CloseHandle(wov.hEvent);if(!ok){alive=false;break;}}
                        }else alive=false;
                    }
                    app_connected=false;
                    post([this]{app_active=false;interest();maybe_exit();});
                }
                CancelIoEx(pipe,nullptr);DisconnectNamedPipe(pipe);CloseHandle(pipe);CloseHandle(read_event);
                if(WaitForSingleObject(stop_pipe,0)==WAIT_OBJECT_0)return;
            }
        });
    }
    void start_pipe(PSECURITY_DESCRIPTOR descriptor){
        start_link(descriptor);
        pipe_thread=std::thread([this,descriptor]{
            SECURITY_ATTRIBUTES attributes{sizeof(attributes),descriptor,FALSE};
            for(;;){
                HANDLE pipe=CreateNamedPipeW(pipe_name.c_str(),PIPE_ACCESS_INBOUND|FILE_FLAG_OVERLAPPED,PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,64,64,0,&attributes);
                if(pipe==INVALID_HANDLE_VALUE)return;
                HANDLE event=CreateEventW(nullptr,TRUE,FALSE,nullptr);OVERLAPPED ov{};ov.hEvent=event;
                BOOL connected=ConnectNamedPipe(pipe,&ov);DWORD error=connected?ERROR_SUCCESS:GetLastError();
                HANDLE waits[]={stop_pipe,event};
                bool ready=connected||error==ERROR_PIPE_CONNECTED;
                if(error==ERROR_IO_PENDING)ready=WaitForMultipleObjects(2,waits,FALSE,INFINITE)==WAIT_OBJECT_0+1;
                if(ready){ResetEvent(event);DWORD page{},bytes{};BOOL read=ReadFile(pipe,&page,sizeof(page),&bytes,&ov);if(!read&&GetLastError()==ERROR_IO_PENDING&&WaitForMultipleObjects(2,waits,FALSE,1000)==WAIT_OBJECT_0+1)read=GetOverlappedResult(pipe,&ov,&bytes,FALSE);
                    if(read&&bytes==sizeof(page)&&page<=2)post([this,page]{open(static_cast<int>(page));});}
                CancelIoEx(pipe,nullptr);DisconnectNamedPipe(pipe);CloseHandle(pipe);CloseHandle(event);
                if(WaitForSingleObject(stop_pipe,0)==WAIT_OBJECT_0)return;
            }
        });
    }
    static LRESULT CALLBACK window_proc(HWND window,UINT message,WPARAM wparam,LPARAM lparam){
        auto* self=reinterpret_cast<App*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(message==WM_NCCREATE){self=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(!self)return DefWindowProcW(window,message,wparam,lparam);
        try{
            if(message==AppDispatchMessage){self->dispatch();return 0;}
            if(message==StateMessage){self->changed();return 0;}
            if(message==WM_DEVICECHANGE){self->device_event(wparam,lparam);return TRUE;}
            if(message==WM_POWERBROADCAST){if(wparam==PBT_APMSUSPEND)self->controller->lifecycle(Operation::Suspend);else if(wparam==PBT_APMRESUMEAUTOMATIC||wparam==PBT_APMRESUMESUSPEND)self->controller->lifecycle(Operation::Reconnect);return TRUE;}
            if(message==WM_TIMER&&window==self->dispatcher){KillTimer(window,1);self->maybe_exit();return 0;}
        }catch(const winrt::hresult_error& e){self->diagnostic(winrt::to_string(e.message()));}catch(const std::exception& e){self->diagnostic(e.what());}
        return DefWindowProcW(window,message,wparam,lparam);
    }
};

WidgetProvider::WidgetProvider(App& owner):app(owner){++app.references;app.post([&owner]{owner.provider_started=true;owner.recover();});}
WidgetProvider::~WidgetProvider(){--app.references;app.post([owner=&app]{owner->maybe_exit();});}
void WidgetProvider::CreateWidget(wp::WidgetContext context){auto id=std::wstring(context.Id());auto size=context.Size();app.post([owner=&app,id,size]{owner->widgets[id]={size,false};owner->update_widgets();});}
void WidgetProvider::DeleteWidget(winrt::hstring const& id,winrt::hstring const&){app.post([owner=&app,id=std::wstring(id)]{owner->widgets.erase(id);owner->interest();});}
void WidgetProvider::OnActionInvoked(wp::WidgetActionInvokedArgs args){auto id=std::wstring(args.WidgetContext().Id());auto verb=std::wstring(args.Verb());app.post([owner=&app,id,verb]{owner->action(id,verb);});}
void WidgetProvider::OnWidgetContextChanged(wp::WidgetContextChangedArgs args){auto context=args.WidgetContext();auto id=std::wstring(context.Id());auto size=context.Size();app.post([owner=&app,id,size]{owner->widgets[id].size=size;owner->update_widgets();});}
void WidgetProvider::OnCustomizationRequested(wp::WidgetCustomizationRequestedArgs args){auto context=args.WidgetContext();auto id=std::wstring(context.Id());auto size=context.Size();app.post([owner=&app,id,size]{owner->widgets[id].size=size;owner->open(0);});} // customizing happens in the app
void WidgetProvider::Activate(wp::WidgetContext context){auto id=std::wstring(context.Id());auto size=context.Size();app.post([owner=&app,id,size]{auto& widget=owner->widgets[id];widget.size=size;widget.active=true;owner->interest();owner->update_widgets();});}
void WidgetProvider::Deactivate(winrt::hstring id){app.post([owner=&app,id=std::wstring(id)]{auto found=owner->widgets.find(id);if(found!=owner->widgets.end())found->second.active=false;owner->interest();});}

struct ProviderFactory:winrt::implements<ProviderFactory,IClassFactory>{
    App& app;explicit ProviderFactory(App& owner):app(owner){}
    HRESULT __stdcall CreateInstance(IUnknown* outer,REFIID iid,void** value) noexcept override{if(!value)return E_POINTER;*value=nullptr;if(outer)return CLASS_E_NOAGGREGATION;try{return winrt::make<WidgetProvider>(app).as(iid,value);}catch(...){return winrt::to_hresult();}}
    HRESULT __stdcall LockServer(BOOL lock) noexcept override{if(lock)++app.references;else --app.references;return S_OK;}
};

int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR arguments,int){
    bool provider=wcsstr(arguments,L"--provider")||wcsstr(arguments,L"-Embedding");
    bool startup=false;
    try{
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        // "Start with Windows" (0.6): the startup task launches this process with no arguments, so ask how it was activated.
        if(!provider)try{if(auto activated=winrt::Windows::ApplicationModel::AppInstance::GetActivatedEventArgs())startup=activated.Kind()==winrt::Windows::ApplicationModel::Activation::ActivationKind::StartupTask;}catch(...){}
        INITCOMMONCONTROLSEX common{sizeof(common),ICC_STANDARD_CLASSES|ICC_BAR_CLASSES};InitCommonControlsEx(&common);
        HANDLE token{};winrt::check_bool(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token));DWORD bytes{};GetTokenInformation(token,TokenUser,nullptr,0,&bytes);std::vector<BYTE> buffer(bytes);winrt::check_bool(GetTokenInformation(token,TokenUser,buffer.data(),bytes,&bytes));CloseHandle(token);
        LPWSTR raw_sid{};winrt::check_bool(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid,&raw_sid));std::wstring sid(raw_sid);LocalFree(raw_sid);
        PSECURITY_DESCRIPTOR descriptor{};winrt::check_bool(ConvertStringSecurityDescriptorToSecurityDescriptorW((L"D:P(A;;GA;;;"+sid+L")").c_str(),SDDL_REVISION_1,&descriptor,nullptr));
        SECURITY_ATTRIBUTES attributes{sizeof(attributes),descriptor,FALSE};DWORD session_id{};ProcessIdToSessionId(GetCurrentProcessId(),&session_id);
        auto suffix=sid+L"."+std::to_wstring(session_id);auto pipe=L"\\\\.\\pipe\\EarbudControls."+suffix;
        HANDLE mutex=CreateMutexW(&attributes,FALSE,(L"Local\\EarbudControls."+suffix).c_str());if(!mutex)winrt::throw_last_error();
        if(GetLastError()==ERROR_ALREADY_EXISTS){
            if(!provider){auto deadline=Clock::now()+2s;while(Clock::now()<deadline){HANDLE client=CreateFileW(pipe.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);if(client!=INVALID_HANDLE_VALUE){DWORD page=0,written{};WriteFile(client,&page,sizeof(page),&written,nullptr);CloseHandle(client);break;}std::this_thread::sleep_for(50ms);}}
            LocalFree(descriptor);CloseHandle(mutex);return 0;
        }
        {
            App app;app.pipe_name=pipe;app.start_in_tray=startup;app.init(!provider);app.start_pipe(descriptor);
            auto factory=winrt::make<ProviderFactory>(app);DWORD cookie{};
            winrt::check_hresult(CoRegisterClassObject(ProviderClsid,factory.get(),CLSCTX_LOCAL_SERVER,REGCLS_MULTIPLEUSE,&cookie));
            MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0){if(app.editor&&IsDialogMessageW(app.editor,&msg))continue;TranslateMessage(&msg);DispatchMessageW(&msg);}
            CoRevokeClassObject(cookie);
        }
        LocalFree(descriptor);CloseHandle(mutex);return 0;
    }catch(const winrt::hresult_error& e){if(!provider)MessageBoxW(nullptr,e.message().c_str(),L"Sound Connect",MB_ICONERROR);return 1;}
    catch(const std::exception& e){if(!provider)MessageBoxW(nullptr,winrt::to_hstring(e.what()).c_str(),L"Sound Connect",MB_ICONERROR);return 1;}
}


