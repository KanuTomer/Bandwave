#pragma once
#include "widget_card.hpp"
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <cmath>

class NativeEditor {
public:
    HWND window{};
    std::function<Snapshot()> read;
    std::function<void(Command)> send;
    std::function<void()> closed;
    std::function<bool()> get_logging;
    std::function<void(bool)> set_logging;
    NativeEditor(std::function<Snapshot()> reader,std::function<void(Command)> sender,std::function<void()> on_close,
                 std::function<bool()> logging_reader,std::function<void(bool)> logging_writer)
        :read(std::move(reader)),send(std::move(sender)),closed(std::move(on_close)),get_logging(std::move(logging_reader)),set_logging(std::move(logging_writer)) {
        background_=CreateSolidBrush(RGB(248,249,251));
        WNDCLASSW wc{};wc.lpfnWndProc=proc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"EarbudControls.NativeEditor";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassW(&wc);
        window=CreateWindowExW(WS_EX_CONTROLPARENT,wc.lpszClassName,L"Earbud Controls",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,650,840,nullptr,nullptr,wc.hInstance,this);
        if(!window)winrt::throw_last_error();
        wc.lpszClassName=L"EarbudControls.Page";RegisterClassW(&wc);
        viewport_=CreateWindowExW(WS_EX_CONTROLPARENT,wc.lpszClassName,L"",WS_CHILD|WS_VISIBLE|WS_VSCROLL|WS_CLIPCHILDREN,0,0,0,0,window,nullptr,wc.hInstance,this);
        theme();
        add(L"STATIC",L"WF-C710N",10,24,18,-165,30);
        add(L"STATIC",L"Connecting...",11,24,54,-48,24);
        add(L"STATIC",L"",12,24,82,-48,24);
        add(L"BUTTON",L"Retry",103,-126,18,102,34,WS_TABSTOP|BS_OWNERDRAW);
        add(L"BUTTON",L"Sound",200,24,120,138,36,WS_TABSTOP|BS_OWNERDRAW);
        add(L"BUTTON",L"Device",201,172,120,138,36,WS_TABSTOP|BS_OWNERDRAW);
        add(L"BUTTON",L"Settings",202,320,120,138,36,WS_TABSTOP|BS_OWNERDRAW);

        add(L"STATIC",L"Noise control",300,24,8,-48,24,0,0);
        add(L"BUTTON",L"Noise cancelling",100,24,38,178,36,WS_TABSTOP|BS_OWNERDRAW,0);
        add(L"BUTTON",L"Ambient",101,212,38,132,36,WS_TABSTOP|BS_OWNERDRAW,0);
        add(L"BUTTON",L"Off",102,354,38,96,36,WS_TABSTOP|BS_OWNERDRAW,0);
        add(L"STATIC",L"Ambient level",301,24,90,104,30,0,0);
        slider(400,L"Ambient level",128,86,-80,36,1,20,0);
        add(L"STATIC",L"--",401,-62,90,42,30,0,0);
        add(L"STATIC",L"Equalizer",302,24,140,-48,24,0,0);
        add(L"COMBOBOX",L"Equalizer preset",410,24,174,-170,220,WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,0);
        add(L"BUTTON",L"Reset edits",411,-140,172,116,34,WS_TABSTOP|BS_OWNERDRAW,0);
        add(L"STATIC",L"Choose Manual, Custom 1, or Custom 2 to edit its bands.",412,24,218,-48,34,0,0);
        const wchar_t* bands[]={L"400 Hz",L"1 kHz",L"2.5 kHz",L"6.3 kHz",L"16 kHz"};
        for(int i=0;i<5;++i){
            int y=258+i*45;
            add(L"STATIC",bands[i],430+i,24,y,86,30,0,0);
            slider(500+i,bands[i],112,y-4,-80,36,-10,10,0);
            add(L"STATIC",L"--",550+i,-62,y,42,30,0,0);
        }
        add(L"STATIC",L"Clear Bass",303,24,492,90,30,0,0);
        slider(520,L"Clear Bass",112,488,-80,36,-10,10,0);
        add(L"STATIC",L"--",570,-62,492,42,30,0,0);
        add(L"STATIC",L"",419,24,542,-48,68,0,0);

        add(L"STATIC",L"",600,24,12,-48,190,0,1);
        add(L"STATIC",L"",601,24,214,-48,170,0,1);
        add(L"BUTTON",L"Windows Bluetooth settings",104,24,400,254,36,WS_TABSTOP|BS_OWNERDRAW,1);
        add(L"STATIC",L"Appearance",610,24,12,-48,24,0,2);
        add(L"STATIC",L"Follows Windows theme, accent, and text size.",611,24,48,-48,46,0,2);
        add(L"BUTTON",L"Enable diagnostic logging (maximum 5 MiB)",310,24,112,-48,40,WS_TABSTOP|BS_AUTOCHECKBOX,2);
        add(L"STATIC",L"Logging stays on this laptop. No audio is recorded.",612,24,158,-48,46,0,2);
        add(L"BUTTON",L"Windows Bluetooth settings",105,24,234,254,36,WS_TABSTOP|BS_OWNERDRAW,2);
        add(L"STATIC",L"Add WF-C710N Controls from Win + W > Add widgets.\r\n\r\nClose these controls when finished. The earbud connection is released after 15 seconds of inactivity.",613,24,302,-48,130,0,2);
        theme();
        RECT work{};SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);
        SetWindowPos(window,nullptr,0,0,std::min<int>(static_cast<int>(660*scale_),work.right-work.left-48),std::min<int>(static_cast<int>(840*scale_),work.bottom-work.top-48),SWP_NOMOVE|SWP_NOZORDER);
        layout();refresh();
    }
    ~NativeEditor(){if(window)DestroyWindow(window);if(font_)DeleteObject(font_);if(title_font_)DeleteObject(title_font_);if(background_)DeleteObject(background_);}
    void show(int page){page_=std::clamp(page,0,2);scroll_=0;layout();ShowWindow(window,SW_RESTORE);SetForegroundWindow(window);refresh();}
    void refresh(){
        if(!window)return;
        auto s=read();
        text(11,s.pending&&s.control!=ControlState::Opening?"Applying...":s.ready?"Connected":status_word(s));
        text(12,battery_text(s));
        text(300,std::string("Noise control: ")+mode_name(s.noise)+(s.fresh?"":" (last checked)"));
        for(int id=100;id<=102;++id){EnableWindow(control(id),s.ready&&s.noise_supported&&!s.pending);InvalidateRect(control(id),nullptr,TRUE);}
        EnableWindow(control(103),!s.pending);
        bool editable=s.preset==MDR_EQ_CUSTOM||s.preset==MDR_EQ_USER_1||s.preset==MDR_EQ_USER_2;
        for(int i=0;i<5;++i)EnableWindow(control(500+i),s.ready&&s.eq_supported&&s.bands.size()==5&&editable&&!s.pending);
        EnableWindow(control(520),s.ready&&s.eq_supported&&s.bands.size()==5&&editable&&!s.pending);
        EnableWindow(control(400),s.ready&&s.ambient_supported&&s.noise==MDR_NOISE_MODE_AMBIENT&&!s.pending);
        EnableWindow(control(410),s.ready&&s.eq_supported&&!s.pending);
        EnableWindow(control(411),baseline_.has_value()&&s.ready&&!s.pending);
        if(s.presets!=presets_&&!SendMessageW(control(410),CB_GETDROPPEDSTATE,0,0)){
            presets_=s.presets;SendMessageW(control(410),CB_RESETCONTENT,0,0);
            for(auto& [id,name]:presets_)SendMessageW(control(410),CB_ADDSTRING,0,reinterpret_cast<LPARAM>(winrt::to_hstring(name).c_str()));
        }
        if(!s.pending&&!debounce_&&!dragging_){
            for(size_t i=0;i<presets_.size();++i)if(presets_[i].first==s.preset)SendMessageW(control(410),CB_SETCURSEL,i,0);
            if(s.bands.size()==5)for(int i=0;i<5;++i)SendMessageW(control(500+i),TBM_SETPOS,TRUE,s.bands[static_cast<size_t>(i)]);
            SendMessageW(control(520),TBM_SETPOS,TRUE,s.bass);
            if(s.ambient>=1&&s.ambient<=20)SendMessageW(control(400),TBM_SETPOS,TRUE,s.ambient);
        }
        values();
        if(!s.eq_supported||s.bands.size()!=5){for(int i=0;i<5;++i)text(550+i,"--");text(570,"--");}
        if(s.ambient<1)text(401,"--");
        auto help=s.error.empty()?"Changes apply to the earbuds. Reset restores edits to the current preset.":s.error;
        text(419,help);
        text(600,"Device: WF-C710N\r\nFirmware: "+(s.firmware.empty()?std::string("Not checked"):s.firmware)+"\r\nControls: "+std::string(s.ready?"Connected":status_word(s))+"\r\n"+battery_text(s)+"\r\n"+checked_time(s.checked));
        text(601,s.error.empty()?std::string("Noise control: ")+(s.noise_supported?"Available":"Not verified")+"\r\nAmbient sound: "+(s.ambient_supported?"Available":"Not verified")+"\r\nEqualizer: "+(s.eq_supported?"Available":"Not verified"):s.error+"\r\n\r\nIf the phone app is open, close it and select Retry.");
        SendMessageW(control(310),BM_SETCHECK,get_logging()?BST_CHECKED:BST_UNCHECKED,0);
        if(!s.ready)baseline_.reset();
    }
private:
    struct Placement{HWND hwnd;int page,x,y,width,height,id;};
    HWND viewport_{};
    std::vector<Placement> placements_;
    std::map<int,HWND> handles_;
    std::vector<std::pair<uint32_t,std::string>> presets_;
    std::optional<Snapshot> baseline_;
    HFONT font_{},title_font_{};HBRUSH background_{};
    COLORREF background_color_=RGB(248,249,251),text_color_=RGB(24,24,24),accent_=RGB(0,103,192),button_=RGB(233,235,238);
    float scale_=1;
    int page_{},scroll_{},debounce_id_{};
    bool dark_{},high_contrast_{},debounce_{},dragging_{};
    HWND control(int id)const{auto found=handles_.find(id);return found==handles_.end()?nullptr:found->second;}
    void text(int id,const std::string& value){SetWindowTextW(control(id),winrt::to_hstring(value).c_str());}
    void add(const wchar_t* cls,const wchar_t* label,int id,int x,int y,int width,int height,DWORD style=0,int page=-1){
        auto hwnd=CreateWindowExW(0,cls,label,WS_CHILD|WS_VISIBLE|style,0,0,0,0,page<0?window:viewport_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
        handles_[id]=hwnd;placements_.push_back({hwnd,page,x,y,width,height,id});SendMessageW(hwnd,WM_SETFONT,reinterpret_cast<WPARAM>(id==10?title_font_:font_),TRUE);
        if(style&WS_TABSTOP)SetWindowSubclass(hwnd,focus_proc,1,reinterpret_cast<DWORD_PTR>(this));
    }
    void slider(int id,const wchar_t* label,int x,int y,int width,int height,int minimum,int maximum,int page){
        add(TRACKBAR_CLASSW,label,id,x,y,width,height,WS_TABSTOP|TBS_AUTOTICKS|TBS_HORZ,page);
        SendMessageW(control(id),TBM_SETRANGEMIN,FALSE,minimum);SendMessageW(control(id),TBM_SETRANGEMAX,TRUE,maximum);SendMessageW(control(id),TBM_SETTICFREQ,1,0);
    }
    void theme(){
        DWORD light=1,text_scale=100,bytes=sizeof(DWORD);
        RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",L"AppsUseLightTheme",RRF_RT_REG_DWORD,nullptr,&light,&bytes);
        bytes=sizeof(DWORD);RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Accessibility",L"TextScaleFactor",RRF_RT_REG_DWORD,nullptr,&text_scale,&bytes);
        scale_=static_cast<float>(GetDpiForWindow(window))/96.0f*static_cast<float>(std::clamp<DWORD>(text_scale,100,225))/100.0f;
        HIGHCONTRASTW hc{sizeof(hc)};SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(hc),&hc,0);high_contrast_=(hc.dwFlags&HCF_HIGHCONTRASTON)!=0;dark_=!light&&!high_contrast_;
        background_color_=high_contrast_?GetSysColor(COLOR_WINDOW):dark_?RGB(31,32,35):RGB(248,249,251);
        text_color_=high_contrast_?GetSysColor(COLOR_WINDOWTEXT):dark_?RGB(242,242,245):RGB(24,24,28);
        button_=high_contrast_?GetSysColor(COLOR_BTNFACE):dark_?RGB(52,54,59):RGB(228,231,236);
        DWORD color{};BOOL opaque{};if(SUCCEEDED(DwmGetColorizationColor(&color,&opaque)))accent_=RGB((color>>16)&255,(color>>8)&255,color&255);
        if(high_contrast_)accent_=GetSysColor(COLOR_HIGHLIGHT);
        if(background_)DeleteObject(background_);background_=CreateSolidBrush(background_color_);
        auto old_font=font_,old_title=title_font_;
        font_=CreateFontW(-static_cast<int>(15*scale_),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        title_font_=CreateFontW(-static_cast<int>(22*scale_),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        for(auto& p:placements_){SendMessageW(p.hwnd,WM_SETFONT,reinterpret_cast<WPARAM>(p.id==10?title_font_:font_),TRUE);SetWindowTheme(p.hwnd,L"Explorer",nullptr);}
        if(old_font)DeleteObject(old_font);if(old_title)DeleteObject(old_title);
        BOOL dark=dark_;DwmSetWindowAttribute(window,DWMWA_USE_IMMERSIVE_DARK_MODE,&dark,sizeof(dark));
        InvalidateRect(window,nullptr,TRUE);if(viewport_)InvalidateRect(viewport_,nullptr,TRUE);
    }
    void layout(){
        if(!viewport_)return;
        RECT client{};GetClientRect(window,&client);int viewport_top=static_cast<int>(172*scale_);
        MoveWindow(viewport_,0,viewport_top,client.right,std::max<int>(1,client.bottom-viewport_top),TRUE);
        RECT view{};GetClientRect(viewport_,&view);int visible=static_cast<int>(view.bottom/scale_);int extent=page_==0?620:page_==1?460:450;
        scroll_=std::clamp(scroll_,0,std::max(0,extent-visible));SCROLLINFO info{sizeof(info),SIF_RANGE|SIF_PAGE|SIF_POS,0,extent-1,static_cast<UINT>(std::max(1,visible)),scroll_,0};SetScrollInfo(viewport_,SB_VERT,&info,TRUE);
        for(auto& p:placements_){
            bool shown=p.page<0||p.page==page_;ShowWindow(p.hwnd,shown?SW_SHOW:SW_HIDE);if(!shown)continue;
            int width=static_cast<int>((p.page<0?client.right:view.right)/scale_);
            int x=p.x>=0?p.x:width+p.x;int w=p.width>0?p.width:width-x+p.width;
            MoveWindow(p.hwnd,static_cast<int>(x*scale_),static_cast<int>((p.y-(p.page<0?0:scroll_))*scale_),std::max(1,static_cast<int>(w*scale_)),static_cast<int>(p.height*scale_),TRUE);
        }
        for(int id=200;id<=202;++id)InvalidateRect(control(id),nullptr,TRUE);
    }
    void values(){for(int i=0;i<5;++i)text(550+i,std::to_string(static_cast<int>(SendMessageW(control(500+i),TBM_GETPOS,0,0)))+" dB");text(570,std::to_string(static_cast<int>(SendMessageW(control(520),TBM_GETPOS,0,0))));text(401,std::to_string(static_cast<int>(SendMessageW(control(400),TBM_GETPOS,0,0))));}
    void apply_slider(int id){
        auto s=read();if(!s.ready||s.pending)return;
        if(id==400){send({Operation::Ambient,static_cast<int>(SendMessageW(control(id),TBM_GETPOS,0,0))});return;}
        if(!baseline_)baseline_=s;
        if(id==520)send({Operation::Bass,static_cast<int>(SendMessageW(control(id),TBM_GETPOS,0,0))});
        else{Command command{Operation::Bands};for(int i=0;i<5;++i)command.bands.push_back(static_cast<int32_t>(SendMessageW(control(500+i),TBM_GETPOS,0,0)));send(std::move(command));}
    }
    void command(int id,int notification){
        if(id>=200&&id<=202){page_=id-200;scroll_=0;layout();return;}
        if(id>=100&&id<=102){send({Operation::Noise,id==100?1:id==101?2:0});return;}
        if(id==103){send({Operation::Open});return;}
        if(id==104||id==105){ShellExecuteW(window,L"open",L"ms-settings:bluetooth",nullptr,nullptr,SW_SHOWNORMAL);return;}
        if(id==310){set_logging(SendMessageW(control(id),BM_GETCHECK,0,0)==BST_CHECKED);return;}
        if(id==410&&notification==CBN_SELCHANGE){auto index=SendMessageW(control(id),CB_GETCURSEL,0,0);if(index>=0&&static_cast<size_t>(index)<presets_.size()){baseline_.reset();send({Operation::Preset,static_cast<int>(presets_[static_cast<size_t>(index)].first)});}return;}
        if(id==411&&baseline_){Command restore{Operation::Reset};restore.bands=baseline_->bands;restore.bass=baseline_->bass;restore.preset=baseline_->preset;send(std::move(restore));baseline_.reset();}
    }
    void draw_button(DRAWITEMSTRUCT const& draw){
        auto state=read();bool selected=(draw.CtlID>=200&&draw.CtlID<=202&&static_cast<int>(draw.CtlID)==200+page_)||(draw.CtlID==100&&state.noise==1)||(draw.CtlID==101&&state.noise==2)||(draw.CtlID==102&&state.noise==0);
        COLORREF fill=selected?accent_:button_;if(draw.itemState&ODS_SELECTED)fill=high_contrast_?GetSysColor(COLOR_HIGHLIGHT):RGB(GetRValue(fill)*4/5,GetGValue(fill)*4/5,GetBValue(fill)*4/5);
        HBRUSH brush=CreateSolidBrush(fill);HPEN pen=CreatePen(PS_SOLID,1,fill);auto old_brush=SelectObject(draw.hDC,brush);auto old_pen=SelectObject(draw.hDC,pen);
        RoundRect(draw.hDC,draw.rcItem.left,draw.rcItem.top,draw.rcItem.right,draw.rcItem.bottom,static_cast<int>(10*scale_),static_cast<int>(10*scale_));SelectObject(draw.hDC,old_brush);SelectObject(draw.hDC,old_pen);DeleteObject(brush);DeleteObject(pen);
        int luminance=GetRValue(fill)*299+GetGValue(fill)*587+GetBValue(fill)*114;
        SetTextColor(draw.hDC,(draw.itemState&ODS_DISABLED)?GetSysColor(COLOR_GRAYTEXT):selected?(luminance>150000?RGB(0,0,0):RGB(255,255,255)):text_color_);SetBkMode(draw.hDC,TRANSPARENT);
        wchar_t caption[128]{};GetWindowTextW(draw.hwndItem,caption,128);RECT rect=draw.rcItem;SelectObject(draw.hDC,font_);DrawTextW(draw.hDC,caption,-1,&rect,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        if(draw.itemState&ODS_FOCUS){InflateRect(&rect,-4,-4);DrawFocusRect(draw.hDC,&rect);}
    }
    void scroll(int code){RECT view{};GetClientRect(viewport_,&view);SCROLLINFO info{sizeof(info),SIF_ALL};GetScrollInfo(viewport_,SB_VERT,&info);int page=static_cast<int>(view.bottom/scale_);
        if(code==SB_LINEUP)scroll_-=32;else if(code==SB_LINEDOWN)scroll_+=32;else if(code==SB_PAGEUP)scroll_-=page;else if(code==SB_PAGEDOWN)scroll_+=page;else if(code==SB_THUMBTRACK)scroll_=info.nTrackPos;layout();}
    static LRESULT CALLBACK focus_proc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam,UINT_PTR id,DWORD_PTR data){
        auto* self=reinterpret_cast<NativeEditor*>(data);
        if(message==WM_SETFOCUS){for(auto& p:self->placements_)if(p.hwnd==hwnd&&p.page>=0){RECT view{};GetClientRect(self->viewport_,&view);int height=static_cast<int>(view.bottom/self->scale_);if(p.y<self->scroll_)self->scroll_=p.y;if(p.y+p.height>self->scroll_+height)self->scroll_=p.y+p.height-height;self->layout();break;}}
        if(message==WM_NCDESTROY)RemoveWindowSubclass(hwnd,focus_proc,id);
        return DefSubclassProc(hwnd,message,wparam,lparam);
    }
    static LRESULT CALLBACK proc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam){
        auto* self=reinterpret_cast<NativeEditor*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(message==WM_NCCREATE){self=static_cast<NativeEditor*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(!self)return DefWindowProcW(hwnd,message,wparam,lparam);
        try{
            if(message==WM_ERASEBKGND){RECT area{};GetClientRect(hwnd,&area);FillRect(reinterpret_cast<HDC>(wparam),&area,self->background_);return 1;}
            if(message==WM_CTLCOLORSTATIC||message==WM_CTLCOLORBTN){auto dc=reinterpret_cast<HDC>(wparam);SetTextColor(dc,self->text_color_);SetBkColor(dc,self->background_color_);return reinterpret_cast<LRESULT>(self->background_);}
            if(message==WM_DRAWITEM){self->draw_button(*reinterpret_cast<DRAWITEMSTRUCT*>(lparam));return TRUE;}
            if(message==WM_COMMAND){self->command(LOWORD(wparam),HIWORD(wparam));return 0;}
            if(message==WM_HSCROLL&&lparam){int id=GetDlgCtrlID(reinterpret_cast<HWND>(lparam));self->values();int code=LOWORD(wparam);if(GetKeyState(VK_LBUTTON)<0)self->dragging_=true;
                if(code==TB_ENDTRACK&&self->dragging_){self->dragging_=false;KillTimer(self->window,2);self->debounce_=false;self->apply_slider(id);}else if(!self->dragging_){self->debounce_=true;self->debounce_id_=id;SetTimer(self->window,2,250,nullptr);}return 0;}
            if(message==WM_TIMER&&wparam==2){KillTimer(self->window,2);self->debounce_=false;self->apply_slider(self->debounce_id_);return 0;}
            if(message==WM_SIZE&&hwnd==self->window){self->layout();return 0;}
            if(message==WM_VSCROLL&&hwnd==self->viewport_){self->scroll(LOWORD(wparam));return 0;}
            if(message==WM_MOUSEWHEEL){self->scroll(GET_WHEEL_DELTA_WPARAM(wparam)>0?SB_LINEUP:SB_LINEDOWN);return 0;}
            if(message==WM_SETTINGCHANGE){self->theme();self->layout();return 0;}
            if(message==WM_DPICHANGED){auto* rect=reinterpret_cast<RECT*>(lparam);SetWindowPos(hwnd,nullptr,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,SWP_NOZORDER);self->theme();self->layout();return 0;}
            if(message==WM_GETMINMAXINFO){auto* info=reinterpret_cast<MINMAXINFO*>(lparam);info->ptMinTrackSize={static_cast<LONG>(490*self->scale_),static_cast<LONG>(400*self->scale_)};return 0;}
            if(message==WM_CLOSE&&hwnd==self->window){auto s=self->read();if((self->debounce_||(s.pending&&s.control!=ControlState::Opening))&&MessageBoxW(hwnd,L"A setting change is unresolved. Close controls anyway?",L"Earbud Controls",MB_YESNO|MB_ICONQUESTION)!=IDYES)return 0;
                KillTimer(hwnd,2);DestroyWindow(hwnd);self->window=nullptr;self->closed();return 0;}
        }catch(const std::exception& e){MessageBoxW(self->window,winrt::to_hstring(e.what()).c_str(),L"Earbud Controls",MB_ICONERROR);}catch(const winrt::hresult_error& e){MessageBoxW(self->window,e.message().c_str(),L"Earbud Controls",MB_ICONERROR);}
        return DefWindowProcW(hwnd,message,wparam,lparam);
    }
};
