#include "widget_card.hpp"
#include "widget_images.hpp"
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <atomic>
#include <functional>
#include <fstream>
#include <filesystem>
#include <set>
using namespace winrt::Windows::Data::Json;
static void expect(bool value,const char* message){if(!value)throw std::runtime_error(message);}
static std::vector<JsonObject> actions(const JsonObject& card){
    std::vector<JsonObject> result;
    std::function<void(IJsonValue)> visit=[&](IJsonValue value){
        if(value.ValueType()==JsonValueType::Object){auto item=value.GetObject();if(item.GetNamedString(L"type",L"")==L"Action.Execute")result.push_back(item);for(auto pair:item)visit(pair.Value());}
        else if(value.ValueType()==JsonValueType::Array)for(auto child:value.GetArray())visit(child);
    };visit(card);return result;
}
static std::pair<int,int> png_dimensions(const std::string& uri){
    constexpr std::string_view alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";std::vector<unsigned char> bytes;uint32_t buffer=0;int bits=0;
    for(char ch:uri.substr(22)){auto value=alphabet.find(ch);if(value==std::string_view::npos)break;buffer=(buffer<<6)|static_cast<uint32_t>(value);bits+=6;if(bits>=8){bits-=8;bytes.push_back(static_cast<unsigned char>(buffer>>bits));if(bytes.size()==24)break;}}
    expect(bytes.size()==24,"PNG must contain an IHDR header.");auto read=[&](int offset){int value=0;for(int i=0;i<4;++i)value=(value<<8)|bytes[offset+i];return value;};return {read(16),read(20)};
}
static std::array<uint8_t,4> png_pixel(const std::string& uri,int x,int y){
    constexpr std::string_view alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";std::vector<uint8_t> bytes;uint32_t buffer=0;int bits=0;
    for(char ch:uri.substr(22)){auto value=alphabet.find(ch);if(value==std::string_view::npos)break;buffer=(buffer<<6)|static_cast<uint32_t>(value);bits+=6;if(bits>=8){bits-=8;bytes.push_back(static_cast<uint8_t>(buffer>>bits));}}
    winrt::com_ptr<IStream> stream;winrt::check_hresult(CreateStreamOnHGlobal(nullptr,TRUE,stream.put()));winrt::check_hresult(stream->Write(bytes.data(),static_cast<ULONG>(bytes.size()),nullptr));LARGE_INTEGER start{};winrt::check_hresult(stream->Seek(start,STREAM_SEEK_SET,nullptr));
    winrt::com_ptr<IWICImagingFactory> factory;winrt::check_hresult(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(factory.put())));
    winrt::com_ptr<IWICBitmapDecoder> decoder;winrt::check_hresult(factory->CreateDecoderFromStream(stream.get(),nullptr,WICDecodeMetadataCacheOnLoad,decoder.put()));winrt::com_ptr<IWICBitmapFrameDecode> frame;winrt::check_hresult(decoder->GetFrame(0,frame.put()));
    winrt::com_ptr<IWICFormatConverter> converter;winrt::check_hresult(factory->CreateFormatConverter(converter.put()));winrt::check_hresult(converter->Initialize(frame.get(),GUID_WICPixelFormat32bppBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom));
    WICRect area{x,y,1,1};std::array<uint8_t,4> pixel{};winrt::check_hresult(converter->CopyPixels(&area,4,4,pixel.data()));return pixel;
}
int main(int argc,char** argv){try{
    winrt::init_apartment();
    std::string diagnostic;try{throw std::runtime_error("Earbud Controls diagnostic sentinel");}catch(const std::exception& e){diagnostic=e.what();}
    expect(diagnostic=="Earbud Controls diagnostic sentinel","Native exceptions must remain readable.");
    try{check(MDR_RESULT_ERROR_TIMEOUT,"Diagnostic timeout test");}catch(const std::exception& e){diagnostic=e.what();}
    expect(diagnostic=="Diagnostic timeout test: Timed out","Engine errors must remain readable.");
    Snapshot cached;cached.checked=1;cached.batteries={82,-1,-1};cached.noise=2;
    expect(battery_text(cached).find("0%")==std::string::npos,"Missing battery must not become zero.");
    expect(battery_text(cached).find("R --")!=std::string::npos,"Missing right battery must remain unknown.");
    Snapshot live=cached;live.ready=live.fresh=live.eq_supported=live.noise_supported=live.ambient_supported=true;live.preset=MDR_EQ_CUSTOM;live.bands={1,0,1,2,3};live.bass=4;
    live.batteries={90,0,50};
    for(auto id:{0,8,9,10,11,12,13,14,15,24,25,26})live.presets.emplace_back(id,preset_name(id));
    WidgetIcons icons{"data:image/png;base64,AA==","data:image/png;base64,AA=="};
    for(auto glyph:{"case","settings","noise-cancelling","ambient","off","equalizer","back","more","previous","next","plus","minus","check","bluetooth","bass","link","diagnostics","log"}){icons.symbols[glyph]={icons.light,icons.dark};icons.symbols[std::string(glyph)+"-selected"]={icons.light,icons.dark};}
    for(int i=0;i<3;++i)icons.batteries[i]={battery_image(live.batteries[i],i==2,false,false),battery_image(live.batteries[i],i==2,true,false)};
    expect(battery_low(0)&&battery_low(19)&&battery_low(20)&&!battery_low(21)&&!battery_low(-1)&&!battery_low(101),"Low battery must include exactly 20 and exclude invalid readings.");
    expect(battery_image(0,false,true,false)!=battery_image(90,false,true,false),"Battery fill must reflect the reported percentage.");
    expect(battery_image(-1,false,true,false)!=battery_image(0,false,true,false),"Unknown and empty batteries must look different.");
    std::filesystem::path export_folder;
    if(argc==3&&std::string_view(argv[1])=="--cards-dir"){
        export_folder=argv[2];std::filesystem::create_directories(export_folder);
        for(auto& [glyph,urls]:icons.symbols){auto base=glyph;bool selected=base.ends_with("-selected");if(selected)base.resize(base.size()-9);
            auto load=[&](const char* theme){std::ifstream in(export_folder/"icons"/(base+"-"+theme+".png"),std::ios::binary);std::string data((std::istreambuf_iterator<char>(in)),{});expect(!data.empty(),"Review artwork must exist.");return png_uri({reinterpret_cast<const uint8_t*>(data.data()),data.size()});};urls={load(selected?"accent-light":"light"),load(selected?"accent":"dark")};
        }
    }
    // The six main-screen EQs, in the order of the app's library; the playing one is marked.
    std::vector<WidgetEq> eqs{{1,"Off",false},{5,"Custom 1 (earbuds)",false},{6,"Custom 2 (earbuds)",true},{2,"Bright",false},{3,"Excited",false},{4,"Mellow",false}};
    for(auto size:{WidgetSize::Small,WidgetSize::Medium,WidgetSize::Large}){
        for(bool pending:{false,true}){
            auto state=cached;state.pending=pending;
            auto card=make_card(state,size,eqs,icons);auto parsed=JsonObject::Parse(winrt::to_hstring(card));
            expect(card.find("tooltip")==std::string::npos,"Routine actions must not obscure adjacent controls with explicit tooltips.");
            expect(card.size()<128*1024,"Card payload must stay bounded.");
            int noise=0;std::set<std::wstring> ids;
            for(auto button:actions(parsed)){
                auto verb=std::wstring(button.GetNamedString(L"verb"));
                expect(ids.insert(std::wstring(button.GetNamedString(L"id"))).second,"Action IDs must remain unique.");
                expect(widget_action_allowed(size,verb),"Every action on a card must be one the provider accepts.");
                if(verb.starts_with(L"noise.")){++noise;expect(!button.GetNamedBoolean(L"isEnabled"),"Stale noise controls must be disabled.");}
                if(verb.starts_with(L"eq."))expect(!button.GetNamedBoolean(L"isEnabled"),"Stale EQ choices must be disabled.");
            }
            expect(noise==3,"Every size exposes the three sound modes.");
            expect(card.find("Last seen")==std::string::npos&&card.find("\"retry\"")==std::string::npos,"The widget reports connection state without a manual retry.");
            if(!pending)expect(card.find("Not connected")!=std::string::npos&&card.find("Disconnected")==std::string::npos&&card.find("Reconnect")==std::string::npos,"Unreachable earbuds must read Not connected, Windows' own word.");
        }
        auto card=make_card(live,size,eqs,icons);auto parsed=JsonObject::Parse(winrt::to_hstring(card));
        int choices=0,enabled=0;std::set<std::wstring> ids;
        for(auto button:actions(parsed)){auto verb=std::wstring(button.GetNamedString(L"verb"));expect(ids.insert(std::wstring(button.GetNamedString(L"id"))).second,"Live action IDs must remain unique.");
            if(verb.starts_with(L"eq.profile.")){++choices;if(button.GetNamedBoolean(L"isEnabled"))++enabled;}}
        expect(choices==(size==WidgetSize::Small?0:6)&&enabled==choices,"Medium and Large offer the six main-screen EQs; Small offers none.");
        if(size!=WidgetSize::Small)expect(card.find("Custom 2 (earbuds), selected")!=std::string::npos,"The EQ the earbuds are playing is marked for screen readers too.");
        expect(card.find("data:image/png;base64,")!=std::string::npos,"Cards must use local artwork.");
        if(!export_folder.empty()){auto name=size==WidgetSize::Small?"small":size==WidgetSize::Large?"large":"medium";
            for(int variant=0;variant<3;++variant){auto state=live;state.noise=variant==0?2:1;if(variant==2){state.ready=state.fresh=false;}std::ofstream out(export_folder/(std::string(name)+"-home-"+std::to_string(variant)+".json"));out<<make_card(state,size,eqs,icons);}}
    }
    // Only sound modes and the six EQs are accepted. Every older editing or navigation verb, and
    // any EQ choice from Small, is refused, so a stale card still on screen cannot write anything.
    for(auto verb:{L"view.eq",L"view.presets",L"view.band.1",L"eq.increase.0",L"eq.decrease.5",L"eq.reset",L"eq.preset.26",L"ambient.up",L"pref.logging",L"app.exit",L"eq.profile.",L"eq.profile.x",L"eq.profile.0"})
        expect(!widget_action_allowed(WidgetSize::Medium,verb),"Removed or malformed widget verbs must be refused.");
    expect(!widget_action_allowed(WidgetSize::Small,L"eq.profile.3")&&widget_action_allowed(WidgetSize::Medium,L"eq.profile.3")&&widget_action_allowed(WidgetSize::Small,L"noise.anc"),"Small keeps one-click sound modes and no EQ choices.");
    auto threshold=live;threshold.batteries={20,21,-1};auto threshold_icons=icons;for(int i=0;i<3;++i)threshold_icons.batteries[i]={battery_image(threshold.batteries[i],i==2,false,false),battery_image(threshold.batteries[i],i==2,true,false)};
    auto threshold_card=make_card(threshold,WidgetSize::Medium,eqs,threshold_icons);auto threshold_json=JsonObject::Parse(winrt::to_hstring(threshold_card));
    std::map<std::wstring,std::wstring> readout_colors;std::function<void(IJsonValue)> collect=[&](IJsonValue value){if(value.ValueType()==JsonValueType::Object){auto item=value.GetObject();if(item.GetNamedString(L"type",L"")==L"TextBlock")readout_colors[std::wstring(item.GetNamedString(L"text",L""))]=std::wstring(item.GetNamedString(L"color",L""));for(auto pair:item)collect(pair.Value());}else if(value.ValueType()==JsonValueType::Array)for(auto child:value.GetArray())collect(child);};collect(threshold_json);
    expect(readout_colors[L"20%"]==L"Attention"&&readout_colors[L"21%"]==L"Default"&&readout_colors.contains(L"--")&&threshold_card.find("Left battery 20%, low")!=std::string::npos&&threshold_card.find("Case battery not reported")!=std::string::npos,"Readings must be host text, with Attention only at or below 20%.");
    readout_colors.clear();collect(JsonObject::Parse(winrt::to_hstring(make_card(live,WidgetSize::Small,eqs,icons))));
    expect(!readout_colors.contains(L"Noise")&&!readout_colors.contains(L"cancelling")&&!readout_colors.contains(L"Ambient"),"Small mode buttons must contain icons only, with names retained on their actions.");
    expect(readout_colors.contains(L"Left")&&readout_colors.contains(L"Right")&&readout_colors.contains(L"Case"),"Small battery labels must be separate from the graphical readings.");
    expect(suffix_number(L"eq.profile.12",L"eq.profile.")==12&&suffix_number(L"eq.profile.99999999999",L"eq.profile.")<0,"EQ ids in actions are bounded.");
    std::atomic<int> notifications{};{ControlSession controller("",*bandwave::profile_by_id("wf-c710n"),[&]{++notifications;},live);expect(!controller.send({Operation::Noise,1})&&!controller.send({Operation::Open}),"Inactive controls must reject hardware actions.");}
    {   // Each device has its own lock, so one device's session never blocks another's; the probe uses the same name.
        expect(transport_lock_name(L"S-1-5",  "AA:BB:CC:DD:EE:01")==L"Local\\EarbudControls.Transport.S-1-5.AABBCCDDEE01"&&transport_lock_name(L"S-1-5","AA:BB:CC:DD:EE:01")!=transport_lock_name(L"S-1-5","AA:BB:CC:DD:EE:02"),"The transport lock must be per device.");
        // Status words: Windows' word for the link, or what is wrong with our channel. Never "Reconnecting" or "Disconnected".
        Snapshot s;expect(std::string(status_word(s))=="Not connected","A device Windows has not connected is Not connected.");
        s.windows_connected=true;expect(!*status_word(s),"A connected device with idle controls says nothing.");
        s.control=ControlState::Opening;expect(std::string(status_word(s))=="Reading…","Opening the channel reads.");
        s.control=ControlState::InUse;expect(std::string(status_word(s))=="Controls in use by another app","A refused channel is in use.");
        s.control=ControlState::Unavailable;expect(std::string(status_word(s))=="Controls unavailable","A failed channel is unavailable.");
        s.control=ControlState::Ready;s.ready=true;expect(!*status_word(s)&&std::string(control_name(s.control))=="ready","Ready controls say nothing.");
        ControlSession other("AA:BB:CC:DD:EE:02",*bandwave::profile_by_id("wf-c710n"),[]{});expect(other.snapshot().address=="AA:BB:CC:DD:EE:02"&&other.snapshot().model=="wf-c710n"&&other.address()=="AA:BB:CC:DD:EE:02","A session belongs to one address and one profile.");
    }
    expect(notifications==0,"An unused controller must produce no background updates.");
    std::cout<<"Passed: widget sizes, sound modes, six main-screen EQs, refused stale verbs, battery readings, inactive lifecycle.\n";return 0;
}catch(const winrt::hresult_error& error){std::cerr<<winrt::to_string(error.message())<<"\n";return 1;}catch(const std::exception& error){std::cerr<<error.what()<<"\n";return 1;}}
