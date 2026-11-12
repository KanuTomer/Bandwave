#pragma once
#include "controller.hpp"
#include <winrt/Microsoft.Windows.Widgets.h>
using winrt::Microsoft::Windows::Widgets::WidgetSize;
#include <map>
#include <optional>

// The widget does three things: batteries, sound mode, and choosing one of the six EQs on the
// app's main screen. Editing lives in the app, so every older EQ, band and page verb is refused.
struct WidgetIcons {
    std::string light,dark;
    std::map<std::string,std::pair<std::string,std::string>> symbols;
    std::array<std::pair<std::string,std::string>,3> batteries;
    std::pair<std::string,std::string> marker,surface,border;
};
// One of the app's six main-screen EQs, as the widget shows it.
struct WidgetEq { uint32_t id{}; std::string name; bool selected{}; };

static bool battery_low(int value){return value>=0&&value<=20;}
static int suffix_number(std::wstring_view verb,std::wstring_view prefix){
    if(!verb.starts_with(prefix)||verb.size()==prefix.size()||verb.size()>prefix.size()+10)return -1;
    long long value{};for(auto ch:verb.substr(prefix.size())){if(ch<L'0'||ch>L'9')return -1;value=value*10+(ch-L'0');if(value>4'000'000'000LL)return -1;}
    return static_cast<int>(value);
}
// A widget follows the device open, or last open, in the app, unless Windows' Customize menu set it
// to one device. That choice is the widget's custom state, which Windows keeps per widget:
// "" or "follow", or "device:AA:BB:CC:DD:EE:FF".
inline std::string widget_pinned(std::wstring_view state){
    constexpr std::wstring_view prefix=L"device:";
    if(!state.starts_with(prefix)||state.size()!=prefix.size()+17)return {};
    std::string address;for(auto c:state.substr(prefix.size())){if(!((c>=L'0'&&c<=L'9')||(c>=L'A'&&c<=L'F')||c==L':'))return {};address+=static_cast<char>(c);}
    return address;
}
inline std::wstring widget_state_text(const std::string& pinned){return pinned.empty()?std::wstring(L"follow"):L"device:"+std::wstring(pinned.begin(),pinned.end());}
// The device a widget shows: its own while that device is still paired, else the one the app follows.
inline std::string widget_device(const std::string& pinned,const std::string& followed,const std::vector<std::string>& supported){
    if(!pinned.empty()&&std::find(supported.begin(),supported.end(),pinned)!=supported.end())return pinned;
    return followed;
}
// Small has no room for EQs, so an EQ action from it can only be stale or forged.
static bool widget_action_allowed(WidgetSize size,std::wstring_view verb){
    if(verb==L"open.app"||verb==L"customize.follow"||verb==L"customize.done")return true;
    if(verb.starts_with(L"customize.device."))return suffix_number(verb,L"customize.device.")>=0&&suffix_number(verb,L"customize.device.")<16;
    if(verb==L"noise.anc"||verb==L"noise.ambient"||verb==L"noise.off")return true;
    if(verb.starts_with(L"eq.profile."))return size!=WidgetSize::Small&&suffix_number(verb,L"eq.profile.")>0;
    return false;
}
