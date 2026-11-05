#include <cstdio>
#include <cstdlib>
#include "model.hpp"
#include "reactive.hpp"
#include "audio_analysis.hpp"
#include "wave_frame.hpp"

#define CHECK(condition) do { if (!(condition)) { std::printf("FAILED line %d: %s\n", __LINE__, #condition); std::exit(1); } } while (0)

int main() {
    using namespace dusk;
    {
        Model m;
        m.editing = true;
        m.edit(0, 7);
        CHECK(m.dirty());
        CHECK(m.heard()[0] == 7); // the EQ on screen is the one heard, with no extra step
        m.editing = false;
        CHECK(m.heard()[0] == 3); // leaving keeps the draft, and the saved version plays
        CHECK(m.current().draft[0] == 7);
        m.editing = true;
        m.revert();
        CHECK(!m.dirty());
        m.edit(0, 7);
        m.save();
        CHECK(!m.dirty());
        m.editing = false;
        CHECK(m.heard()[0] == 7);
    }
    {
        Model m;
        m.edit(1, 40);
        CHECK(m.current().draft[1] == 10);
        m.select(0);
        m.edit(0, 5);
        m.flat();
        m.save();
        m.erase();
        CHECK(m.profiles[0].factory && m.profiles[0].draft[0] == 0);
    }
    {
        Model m;
        const auto count = m.profiles.size();
        const auto name = m.current().name;
        m.erase();
        CHECK(m.profiles.size() == count - 1);
        m.undo_delete();
        CHECK(m.profiles.size() == count && m.current().name == name);
        m.duplicate();
        CHECK(m.profiles.size() == count + 1 && m.current().name == name + L" copy");
    }
    {
        Model m;
        const auto chosen = m.current().name;
        m.move(8, 2); // from the library onto the main screen
        CHECK(m.profiles[2].name == L"Wall of sound");
        CHECK(m.current().name == chosen);
        const auto last = m.profiles.size() - 1;
        m.move(m.selected, last);
        CHECK(m.selected == last && m.current().name == chosen); // the selection follows its EQ
        m.create();
        m.create();
        CHECK(m.current().name == L"New EQ 2" && m.current().saved == Curve{});
    }
    {
        Model m;
        CHECK(!m.toggle_cycle(0)); // two modes are on, so neither can go
        CHECK(m.toggle_cycle(2));
        CHECK(m.toggle_cycle(0));
        CHECK(!m.toggle_cycle(1));
    }
    {
        CHECK(level_from_db(-80) == 0 && level_from_db(0) == 1);
        Envelope e;
        for (int i = 0; i < 60; ++i) e.step(0);
        CHECK(e.value == 0); // silence never moves the background
        e.step(1);
        const float first = e.value;
        CHECK(first > 0.2f); // rises quickly
        for (int i = 0; i < 30; ++i) e.step(1);
        const float peak = e.value;
        e.step(0);
        CHECK(e.value > peak * 0.9f); // falls slowly
        Envelope wall;
        for (int i = 0; i < 900; ++i) wall.step(0.9f);
        CHECK(wall.value < 0.45f); // constant loud music settles rather than pinning at full
        wall.step(1.f);
        CHECK(wall.value > 0.32f);
    }
    {
        CHECK(kSpectrum.size() == kBandEdges.size() - 1);
        CHECK(spectrum_color(0) == kSpectrum[0]);
        CHECK(spectrum_color(1.0 / kSpectrum.size()) == kSpectrum[1]);
        CHECK(spectrum_color(1.25) == spectrum_color(0.25)); // wraps round
        CHECK(advance_phase(0.4, 0) == 0.4); // silence holds the colour
        CHECK(advance_phase(0, 0.9f) > advance_phase(0, 0.1f)); // louder moves faster
        CHECK(advance_phase(0.9999, 1) < 0.01);
        CHECK(even_brightness(0x4A6CF4) > even_brightness(0x4CC86E)); // blue gets more than green
        for (size_t i = 0; i < kBands; ++i) CHECK(kBandEdges[i] < kBandEdges[i + 1]);
        const std::array<int, 6> bass{4, 1, 0, 0, 0, 4}, treble{0, 0, 0, 3, 4, 0}, flat{};
        CHECK(eq_bend(bass, -1) == 4); // the first band holds to the window's edge
        CHECK(eq_bend(treble, 1) == 4);
        CHECK(eq_bend(flat, 0.3) == 0);
        const std::array<int, 6> step{0, 0, 10, 0, 0, 0};
        CHECK(eq_bend(step, 0) == 10); // the middle band sits in the middle of the window
    }
    {
        // Equal elapsed music must not change pace when selection animations
        // move the UI timer from 33 ms to 16 ms, or idle uses 50 ms.
        auto run=[](double dt){ReactiveState state;std::array<float,7> levels{.65f,.7f,.8f,.55f,.45f,.6f,.3f};for(double time=0;time<60-1e-7;time+=dt)state.step(levels,std::min(dt,60-time));return state;};
        auto a=run(.016),b=run(.033),c=run(.05);
        CHECK(std::abs(a.energy-b.energy)<.005&&std::abs(a.energy-c.energy)<.005);
        CHECK(std::abs(a.phase-b.phase)<.005&&std::abs(a.phase-c.phase)<.005);
        auto held=a.phase;for(int i=0;i<100;++i)a.step({},.05);CHECK(a.phase==held);
        Slide slide;slide.retarget(200,true);slide.step(.08);const double interrupted=slide.value;
        slide.retarget(400,true);CHECK(slide.value==interrupted);slide.step(.04);CHECK(slide.value>interrupted&&slide.value<400);
        slide.step(.3);CHECK(slide.value==400);slide.retarget(10,false);CHECK(slide.value==10);
        for(double height:{140.,280.,420.}){Plot plot{1000,height};CHECK(plot.y(10)==14);CHECK(plot.y(-10)==height-14);CHECK(plot.x(0)==100&&plot.x(4)==900);}
        AudioAnalysis analyser;std::array<float,7> bands{};int callbacks=0;
        for(int i=0;i<8192;++i)analyser.push(.5f*static_cast<float>(std::sin(i*2*3.141592653589793*1000/48000)),48000,[&](auto const& result){bands=result;++callbacks;});
        CHECK(callbacks==7);CHECK(bands[3]>.8f&&bands[0]<.1f&&bands[6]<.1f);
    }
    {
        Model model;int factory=0;for(auto const& p:model.profiles)if(p.factory)++factory;CHECK(factory==9);
        model.select(model.profiles.size()-1);auto saved=model.current().saved;model.edit(0,8);model.save();CHECK(model.current().saved==saved);auto count=model.profiles.size();model.duplicate();CHECK(model.profiles.size()==count);
        // Sony's candidates never move 400 Hz or 1 kHz, and option 1 is the EQ left as it is.
        CHECK(kGuideFirst[0]==Curve{});for(auto const& set:kGuideSecond)for(auto const& c:set)CHECK(c[0]==0&&c[1]==0);
        {Model m;m.select(0);CHECK(m.current().factory);auto total=m.profiles.size();m.set_live({2,0,0,0,0,0});
         CHECK(m.profiles.size()==total+1&&m.current().name==L"Off (mine)"&&!m.current().factory&&m.current().saved[0]==2&&m.profiles[0].saved==Curve{});
         m.set_live({3,0,0,0,0,0});CHECK(m.profiles.size()==total+1&&m.current().saved==m.current().draft);
         m.select(m.profiles.size()-1);CHECK(!m.current().curve_known);auto before=m.profiles.size();m.set_live({1,1,1,1,1,1});CHECK(m.profiles.size()==before);}
        {Model m;CHECK(m.kept_slot(7)==0&&m.kept[1]==0);CHECK(!m.keep(1,0));CHECK(!m.keep(1,m.profiles.size()-1));
         CHECK(m.keep(1,7)&&m.kept_slot(7)==1&&m.kept[0]==0);m.keep(0,2);m.move(2,9);CHECK(m.kept_slot(9)==0);
         m.select(9);m.erase();CHECK(m.kept[0]==0);m.create();CHECK(m.find(m.current().id)==m.selected);m.duplicate();CHECK(m.current().id!=m.profiles[m.selected-1].id);}
        {Guide guide;CHECK(guide.curve()==Curve{});CHECK(!guide.back());
         guide.choose(2);CHECK((guide.curve()==Curve{0,0,1,2,3,6}));guide.next();CHECK(guide.stage==1&&guide.curve()==kGuideFirst[2]);
         guide.choose(4);CHECK((guide.curve()==Curve{0,0,0,1,2,3})); // Sony's 3 then 9: Clear Bass +3, 6.3k +1, 16k +2
         guide.next();CHECK(guide.reviewing()&&guide.curve()==kGuideSecond[2][3]);guide.next();CHECK(guide.stage==2);
         CHECK(guide.back()&&guide.stage==1&&guide.second==4);guide.back();guide.choose(1);CHECK(guide.second==0&&guide.curve()==kGuideFirst[1]);
         guide.next();guide.next();guide.before=true;guide.retry();CHECK(guide.stage==0&&guide.first==0&&!guide.before&&guide.curve()==Curve{});}
        ReactiveState low,high;std::array<float,7> bass{},treble{};bass[0]=.8f;treble[5]=.8f;
        for(int i=0;i<900;++i){low.step(bass,1.0/30);high.step(treble,1.0/30);}CHECK(std::abs(low.phase-high.phase)>.2);
        auto held=high.phase;for(int i=0;i<300;++i)high.step(treble,1.0/30);CHECK(std::abs(high.phase-held)<.02);
    }
    {
        Model model;const auto original=model.library_label();model.select(8);CHECK(model.library_label()==original);model.create();auto created=model.library_label();CHECK(created!=original&&created.find(L"All ")==0);model.erase();CHECK(model.library_label()==original);model.undo_delete();CHECK(model.library_label()==created);
        for(int count:{27,43,80}){auto points=circle_points(20,30,130,count);double expected=2*130*std::sin(3.141592653589793/count);for(int i=0;i<count;++i){auto a=points[i],b=points[(i+1)%count];CHECK(std::abs(std::hypot(a[0]-b[0],a[1]-b[1])-expected)<1e-8);}}
    }
    {
        WaveFrame frame;frame.sequence=2;frame.width=1280;frame.height=500;CHECK(!frame.valid_after(1));
        for(auto& line:frame.lines)line={{-WaveFrame::padding,100},{float(frame.width+WaveFrame::padding),100}};
        CHECK(frame.valid_after(1)&&!frame.valid_after(2)&&!frame.valid_after(3));CHECK(frame.covers_edges());frame.lines[0].front().x=0;CHECK(!frame.covers_edges());
    }
    std::printf("All Dusk model tests passed\n");
    return 0;
}
