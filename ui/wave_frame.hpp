#pragma once
#include <array>
#include <vector>
#include <cstdint>

namespace dusk {
struct WavePoint { float x{},y{}; };
struct WaveFrame {
    uint64_t sequence{};
    double width{},height{},time{},dpi=1;
    static constexpr float padding=160; // >= three blur radii + stroke radius
    static constexpr int kLines=15;
    std::array<std::vector<WavePoint>,kLines> lines;
    std::array<float,kLines> weight{1,1,1,1,1,1,1,1,1,1,1,1,1,1,1}; // each line's share of the glow (the outer ones fade)
    int count=kLines; // lines in use; the editor's curve uses one
    // The crisp lines (StageChain): each line's opacity and stroke width in DIPs, in one colour.
    std::array<float,kLines> alpha{},stroke{};
    unsigned ink=0xF2F2F2;
    bool open_ends=false; // a line that ends inside the surface, so its glow rounds off around the ends
    bool valid_after(uint64_t previous) const {if(sequence<=previous||width<=0||height<=0)return false;for(int i=0;i<count;++i)if(lines[i].size()<2)return false;return true;}
    bool covers_edges() const {if(open_ends)return true;for(int i=0;i<count;++i){auto const& line=lines[i];if(line.empty()||line.front().x>-padding||line.back().x<width+padding)return false;}return true;}
};
}
