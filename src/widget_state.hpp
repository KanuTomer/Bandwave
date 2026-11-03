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
// Small has no room for EQs, so an EQ action from it can only be stale or forged.
static bool widget_action_allowed(WidgetSize size,std::wstring_view verb){
    if(verb==L"noise.anc"||verb==L"noise.ambient"||verb==L"noise.off")return true;
    if(verb.starts_with(L"eq.profile."))return size!=WidgetSize::Small&&suffix_number(verb,L"eq.profile.")>0;
    return false;
}
