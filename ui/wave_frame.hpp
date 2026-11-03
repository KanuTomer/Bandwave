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
    std::array<std::vector<WavePoint>,9> lines;
    bool open_ends=false; // a line that ends inside the surface, so its glow rounds off around the ends
    bool valid_after(uint64_t previous) const {if(sequence<=previous||width<=0||height<=0)return false;for(auto const& line:lines)if(line.size()<2)return false;return true;}
    bool covers_edges() const {if(open_ends)return true;for(auto const& line:lines)if(line.empty()||line.front().x>-padding||line.back().x<width+padding)return false;return true;}
};
}
