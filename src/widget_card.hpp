#pragma once
#include "widget_state.hpp"
#include <winrt/Microsoft.Windows.Widgets.h>
#include <ctime>
using winrt::Microsoft::Windows::Widgets::WidgetSize;

static std::string checked_time(int64_t seconds){
    if(!seconds)return "Not checked yet";
    std::time_t value=seconds;std::tm local{};localtime_s(&local,&value);
    char text[64]{};std::strftime(text,sizeof(text),"Last seen %H:%M",&local);return text;
}
static std::string battery_text(const Snapshot& s){
    auto percent=[](int n){return n>=0&&n<=100?std::to_string(n)+"%":"--";};
    return "L "+percent(s.batteries[0])+"    R "+percent(s.batteries[1])+"    Case "+percent(s.batteries[2]);
}
static const char* mode_name(int mode){return mode==1?"Noise cancelling":mode==2?"Ambient sound":mode==0?"Off":"Not checked";}
static std::string signed_value(int value){return (value>0?"+":"")+std::to_string(value);}
namespace card {
using Items=std::vector<std::string>;
static std::string list(const Items& items){std::string result="[";for(auto& item:items){if(item.empty())continue;if(result.size()>1)result+=",";result+=item;}return result+"]";}
static std::string theme(const std::pair<std::string,std::string>& urls){return "${$host.hostTheme == 'dark' ? '"+urls.second+"' : '"+urls.first+"'}";}
static std::string text(const std::string& value,const char* size="Small",bool subtle=false,bool bold=false,const char* align="Left",int lines=2,const char* color="Default"){
    return "{\"type\":\"TextBlock\",\"spacing\":\"None\",\"text\":"+::quoted(value)+",\"size\":"+::quoted(size)+",\"color\":"+::quoted(color)+",\"isSubtle\":"+(subtle?"true":"false")+",\"weight\":"+::quoted(bold?"Bolder":"Default")+",\"wrap\":true,\"maxLines\":"+std::to_string(lines)+",\"horizontalAlignment\":"+::quoted(align)+"}";
}
static std::string action(const std::string& verb,const std::string& label,bool enabled=true,const std::string& id=""){
    return "{\"type\":\"Action.Execute\",\"id\":"+::quoted(id.empty()?verb:id)+",\"verb\":"+::quoted(verb)+",\"title\":"+::quoted(label)+",\"isEnabled\":"+(enabled?"true":"false")+"}";
}
static std::string box(const Items& items,int height=0,const std::string& click="",const std::string& background="",const char* type="Container"){
    return "{\"type\":"+::quoted(type)+",\"spacing\":\"None\",\"verticalContentAlignment\":\"Center\",\"items\":"+list(items)+(height?",\"minHeight\":"+::quoted(std::to_string(height)+"px"):"")+(click.empty()?"":",\"selectAction\":"+click)+(background.empty()?"":",\"style\":"+::quoted(background))+"}";
}
static std::string col(const Items& items,const std::string& width="stretch",int height=0,const std::string& click="",const std::string& background=""){
    auto result=box(items,height,click,background,"Column");
    return result.substr(0,result.size()-1)+",\"width\":"+::quoted(width)+"}";
}
static std::string columns(const Items& items,int height=0){return "{\"type\":\"ColumnSet\",\"spacing\":\"None\",\"columns\":"+list(items)+(height?",\"minHeight\":"+::quoted(std::to_string(height)+"px"):"")+"}";}
static std::string gap(int height){return box({},height);}
static std::string pad(int width){return col({},std::to_string(width)+"px");}
static std::string image(const std::pair<std::string,std::string>& urls,int width,int height,const std::string& alt=""){
    if(urls.first.empty()||urls.second.empty())return {};
    return "{\"type\":\"Image\",\"spacing\":\"None\",\"url\":"+::quoted(theme(urls))+",\"width\":"+::quoted(std::to_string(width)+"px")+",\"height\":"+::quoted(std::to_string(height)+"px")+",\"horizontalAlignment\":\"Center\",\"altText\":"+::quoted(alt)+"}";
}
static std::string glyph(const WidgetIcons& icons,const char* name,int size=20){auto found=icons.symbols.find(name);return found==icons.symbols.end()?"":image(found->second,size,size);}
// The host's emphasis surface supplies fill, hover, focus and high-contrast
// colours natively, instead of imitating a button with PNG frames.
// The host adds no inner padding to styled containers, so content is inset
// explicitly to stay inside the tile and its hover highlight.
static std::string tile(const Items& items,int height,const std::string& click,int inset_x=12,int inset_y=8,const char* surface="emphasis"){
    Items inner{inset_x?columns({pad(inset_x),col(items),pad(inset_x)}):columns({col(items)})};
    if(inset_y){inner.insert(inner.begin(),gap(inset_y));inner.push_back(gap(inset_y));}
    return box(inner,height,click,surface);
}
static std::string image_fill(const std::pair<std::string,std::string>& urls,const std::string& alt){
    if(urls.first.empty()||urls.second.empty())return {};
    return "{\"type\":\"Image\",\"spacing\":\"None\",\"url\":"+::quoted(theme(urls))+",\"size\":\"Stretch\",\"altText\":"+::quoted(alt)+"}";
}
static std::string icon_button(const WidgetIcons& icons,const char* name,const std::string& verb,const std::string& label,bool enabled=true,int size=24,const char* surface="emphasis"){
    bool step=verb.starts_with("eq.increase.")||verb.starts_with("eq.decrease.")||verb.starts_with("ambient.");
    if(!step)return col({glyph(icons,name,18)},std::to_string(size)+"px",size,action(verb,label,enabled));
    // Columns stretch to the tallest item in their row. Keep the styled
    // click target inside the column so a taller value row cannot stretch it.
    return col({tile({glyph(icons,name,16)},size,action(verb,label,enabled),0,0,surface)},std::to_string(size)+"px",size);
}
}
static std::string make_card(const Snapshot& s,WidgetSize size,const std::vector<WidgetEq>& eqs={},const WidgetIcons& icons={}){
    using namespace card;
    const bool compact=size==WidgetSize::Small,expanded=size==WidgetSize::Large,enabled=s.ready&&s.fresh&&!s.pending;
    Items body;
    auto add=[&](std::string item,int before=0){if(before)body.push_back(gap(before));body.push_back(std::move(item));};
    auto noise_tiles=[&](){
        Items tiles;const int modes[]={1,2,0};const char* glyphs[]={"noise-cancelling","ambient","off"};const char* verbs[]={"noise.anc","noise.ambient","noise.off"};
        // Small has room for icons only; the other sizes label every mode.
        int height=compact?36:expanded?92:76;
        for(int i=0;i<3;++i){
            bool selected=s.fresh&&s.noise==modes[i],available=enabled&&(i==1?s.ambient_supported:s.noise_supported);if(i)tiles.push_back(pad(4));
            auto symbol=std::string(glyphs[i])+(selected?"-selected":"");auto label=i==1?"Ambient":mode_name(modes[i]);
            const char* tone=selected?"Accent":"Default";
            // Separate lines prevent the native host from keeping this caption
            // on one line and overflowing the fixed-width mode button.
            Items caption=i==0?Items{text("Noise","Small",!available,selected,"Center",1,tone),text("cancelling","Small",!available,selected,"Center",1,tone)}:Items{text(label,"Small",!available,selected,"Center",2,tone)};
            Items inner=compact?Items{glyph(icons,symbol.c_str(),18)}:Items{glyph(icons,symbol.c_str(),24),gap(4),box(caption,32)};
            tiles.push_back(col({tile(inner,height,action(verbs[i],std::string(mode_name(modes[i]))+(selected?", selected":""),available),4,8)}));
        }return columns(tiles,height);
    };
    const char* battery_names[]={"Left","Right","Case"};Items cells;
    for(int i=0;i<3;++i){
        // While connected, an earbud with no reading isn't connected; say so calmly, not as an alarm.
        int value=s.batteries[i];bool valid=value>0&&value<=100,off=i<2&&!valid&&s.ready&&s.fresh,low=battery_low(value)&&valid;auto reading=valid?std::to_string(value)+"%":off?std::string("Not connected"):"--";
        if(i)cells.push_back(pad(compact?8:16));
        // Host typography scales with text size and follows high contrast.
        auto readout=text(reading,off?"Small":compact?"Default":expanded?"ExtraLarge":"Medium",!s.fresh||off,!off,"Left",1,low?"Attention":"Default");
        auto described=std::string(battery_names[i])+(off?" earbud disconnected":" battery "+(valid?std::to_string(value)+"%":std::string("not reported"))+(low?", low":""));
        if(compact)cells.push_back(col({box({text(battery_names[i],"Small",true)},16),readout},"stretch",36));
        else cells.push_back(col({box({text(battery_names[i],"Small",true)},16),gap(4),box({readout},expanded?36:24),gap(expanded?8:4),image(icons.batteries[i],80,3,described)},"stretch"));
    }
    // Status only earns a row when something needs attention. Reconnection is automatic.
    const bool healthy=s.fresh&&s.ready;
    auto status=s.pending&&s.control!=ControlState::Opening?"Updating…":!s.ready?(*status_word(s)?status_word(s):"Not connected"):"Updating…";
    auto status_row=[&](){return columns({col({text(status,"Small",true)},"stretch",16)},16);};
    if(compact){if(s.fresh)add(columns(cells,36));else add(box({status_row()},36));add(noise_tiles(),4);}
    else{
        if(!healthy)add(status_row());
        add(columns(cells),healthy?0:expanded?8:4);
        add(noise_tiles(),expanded?20:12);
        // The six EQs on the app's main screen, in the app's order: three across on Medium, and on
        // Large two across in taller tiles with a heading, so the extra height is used.
        if(expanded&&!eqs.empty())add(text("Equalizer","Small",true),20);
        const size_t across=expanded?2:3;const int chip=expanded?60:36;
        for(size_t row=0;row<eqs.size()&&row<6;row+=across){Items chips;
            for(size_t j=row;j<std::min<size_t>({eqs.size(),row+across,6});++j){auto& eq=eqs[j];if(j>row)chips.push_back(pad(4));
                chips.push_back(col({tile({text(eq.name,expanded?"Default":"Small",!enabled,eq.selected,"Center",1,eq.selected?"Accent":"Default")},chip,action("eq.profile."+std::to_string(eq.id),eq.name+(eq.selected?", selected":""),enabled&&s.eq_supported),4,8)}));}
            while(chips.size()<across*2-1){chips.push_back(pad(4));chips.push_back(col({}));}
            add(columns(chips,chip),row?4:expanded?6:8);
        }
    }
    return "{\"type\":\"AdaptiveCard\",\"version\":\"1.5\",\"$schema\":\"http://adaptivecards.io/schemas/adaptive-card.json\",\"body\":"+list(body)+"}";
}
