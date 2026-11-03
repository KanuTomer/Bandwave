#pragma once
// Turns what the PC is playing into light. The screen holds one colour at a time, and the music
// carries that colour through the whole visible spectrum.
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace dusk {
inline constexpr size_t kBands = 7;
inline constexpr std::array<float, kBands + 1> kBandEdges{30, 90, 220, 500, 1100, 2500, 6000, 16000};
// Clean hues only. A dim yellow reads as olive and a dim cyan as teal, so those would make the
// light look green for nearly half of every cycle.
inline constexpr std::array<unsigned, kBands> kSpectrum{0xE8405C, 0xF0842C, 0x4CC86E, 0x2EB4E6, 0x4A6CF4, 0x9A58F4, 0xE048B4};

// -55..-10 dBFS onto 0..1.
inline float level_from_db(float db) { return std::clamp((db + 55.f) / 45.f, 0.f, 1.f); }

// Heavily compressed music sits near full level all the time, so level alone barely moves.
// Part of the response follows the level and part follows rises above its recent average.
// It rises quickly and falls slowly, so it breathes rather than flashes. Call about 30 times a second.
struct Envelope {
    float value = 0, average = 0;
    float step(float level, double seconds = 1.0 / 30) {
        const double ticks = std::max(0.0, seconds) * 30;
        average += (level - average) * static_cast<float>(1 - std::pow(.98, ticks));
        const float target = std::clamp(0.35f * level + 1.6f * std::max(0.f, level - average), 0.f, 1.f);
        value += (target - value) * static_cast<float>(1 - std::pow(target > value ? .75 : .96, ticks));
        return value;
    }
};

inline unsigned mix(unsigned from, unsigned to, double t) {
    const auto channel = [&](int shift) {
        const double a = (from >> shift) & 255, b = (to >> shift) & 255;
        return static_cast<unsigned>(a + (b - a) * t + .5) << shift;
    };
    return channel(16) | channel(8) | channel(0);
}

// The one colour on screen, at a position round the spectrum. It blends from violet back to red.
inline unsigned spectrum_color(double phase) {
    phase -= std::floor(phase);
    const double position = phase * kSpectrum.size();
    const size_t index = static_cast<size_t>(position) % kSpectrum.size();
    return mix(kSpectrum[index], kSpectrum[(index + 1) % kSpectrum.size()], position - std::floor(position));
}

// How strongly to show a colour so each looks about as bright as the others. Blue and violet are
// perceived as far dimmer than green at the same strength.
inline double even_brightness(unsigned rgb) {
    const auto linear = [](unsigned value) { return std::pow(value / 255.0, 2.2); };
    const double luminance = .2126 * linear((rgb >> 16) & 255) + .7152 * linear((rgb >> 8) & 255) + .0722 * linear(rgb & 255);
    return std::clamp(std::pow(.24 / std::max(luminance, .01), .5), .75, 1.6);
}

// Music moves the colour along, faster when louder. Silence holds it where it is. A tick is about
// a thirtieth of a second, so moderate music goes round the whole spectrum in about 25 seconds.
inline double advance_phase(double phase, float energy, double ticks = 1) {
    if (energy < 0.02f) return phase;
    phase += (0.0006 + 0.003 * energy) * ticks;
    return phase - std::floor(phase);
}

// The same elapsed-time response is used by live WASAPI and silent file tests.
struct ReactiveState {
    std::array<Envelope, kBands> envelopes{};
    std::array<double, kBands> pulse{};
    double phase = 0, energy = 0;
    bool playing = false;
    std::array<double,kBands> baseline{};
    void step(std::array<float, kBands> const& levels, double seconds) {
        // Small integration steps make the envelope insensitive to UI cadence.
        const int count = std::max(1, static_cast<int>(std::ceil(seconds * 300)));
        const double dt = seconds / count;
        playing = *std::max_element(levels.begin(), levels.end()) > .02f;
        for (int n = 0; n < count; ++n) {
            energy = 0;
            for (size_t i = 0; i < kBands; ++i) {
                pulse[i] = envelopes[i].step(levels[i], dt);
                energy += pulse[i] / kBands;
            }
            if(playing){
                double vx=0,vy=0;
                for(size_t i=0;i<kBands;++i){baseline[i]+=(levels[i]-baseline[i])*(1-std::exp(-dt/3));double weight=std::max(0.0,levels[i]-baseline[i])+.12*levels[i];double angle=6.283185307179586*i/kBands;vx+=weight*std::cos(angle);vy+=weight*std::sin(angle);}
                if(std::hypot(vx,vy)>.02){double target=std::atan2(vy,vx)/6.283185307179586;if(target<0)target+=1;double difference=target-phase;difference-=std::floor(difference+.5);if(std::abs(difference)>.015)phase+=difference*(1-std::exp(-dt/.6));phase-=std::floor(phase);}
            }
        }
    }
};

inline std::vector<std::array<double,2>> circle_points(double x,double y,double radius,int count){
    std::vector<std::array<double,2>> points;for(int i=0;i<count;++i){const double angle=6.283185307179586*i/count;points.push_back({x+radius*std::cos(angle),y+radius*std::sin(angle)});}return points;
}

struct Slide {
    double value = 0, from = 0, target = 0, progress = 1;
    void retarget(double next, bool animate) { from = value; target = next; progress = animate ? 0 : 1; if (!animate) value = next; }
    void step(double seconds) { progress = std::min(1.0, progress + seconds / .2); const double t = 1 - std::pow(1 - progress, 3); value = from + (target - from) * t; }
};

struct Plot {
    double width, height;
    static constexpr double thumb = 28;
    double top() const { return thumb / 2; }
    double travel() const { return std::max(1.0, height - thumb); }
    double x(size_t band) const { return (band + .5) * width / 5; }
    double y(double gain) const { return top() + (10 - gain) / 20 * travel(); }
};

// The EQ's shape across the window, as a smooth line through the five bands. x runs -1 to 1.
inline double eq_bend(std::array<int, 6> const& curve, double x) {
    const double position = std::clamp((x + 1) / 2 * 5 - 0.5, 0.0, 4.0);
    const int index = std::min(3, static_cast<int>(position));
    const double t = position - index, eased = t * t * (3 - 2 * t);
    return curve[static_cast<size_t>(index)] + (curve[static_cast<size_t>(index) + 1] - curve[static_cast<size_t>(index)]) * eased;
}
}
