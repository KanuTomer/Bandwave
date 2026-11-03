#pragma once
#include "reactive.hpp"
#include <complex>
#include <vector>
namespace dusk {
// Identical PCM windowing and FFT for file tests and WASAPI loopback.
struct AudioAnalysis {
    static constexpr int kSize = 2048, kHop = 1024;
    std::vector<float> ring = std::vector<float>(kSize), ordered = std::vector<float>(kSize);
    int filled = 0, fresh = 0, write = 0;
    template<class Callback> void push(float sample, float rate, Callback callback) {
        ring[write] = sample; write = (write + 1) % kSize;
        filled = std::min(filled + 1, kSize); ++fresh;
        if (filled == kSize && fresh >= kHop) {
            fresh = 0;
            for (int i = 0; i < kSize; ++i) ordered[i] = ring[(write + i) % kSize];
            callback(analyse(ordered, rate));
        }
    }
    static std::array<float, kBands> analyse(const std::vector<float>& samples, float rate) {
        std::vector<std::complex<float>> x(kSize);
        for (int i = 0; i < kSize; ++i) x[i] = samples[i] * (0.5f - 0.5f * std::cos(6.2831853f * i / (kSize - 1)));
        for (int i = 1, j = 0; i < kSize; ++i) {
            int bit = kSize >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(x[i], x[j]);
        }
        for (int len = 2; len <= kSize; len <<= 1) {
            const std::complex<float> step = std::polar(1.f, -6.2831853f / len);
            for (int i = 0; i < kSize; i += len) {
                std::complex<float> w = 1;
                for (int k = 0; k < len / 2; ++k) {
                    const auto u = x[i + k], v = x[i + k + len / 2] * w;
                    x[i + k] = u + v;
                    x[i + k + len / 2] = u - v;
                    w *= step;
                }
            }
        }
        const float bin = rate / kSize;
        std::array<float, kBands> out{};
        for (size_t b = 0; b < kBands; ++b) {
            float peak = 0;
            for (int k = std::max(1, static_cast<int>(kBandEdges[b] / bin)); k <= std::min(kSize / 2 - 1, static_cast<int>(kBandEdges[b + 1] / bin)); ++k)
                peak = std::max(peak, std::abs(x[k]) / (kSize / 4.f));
            out[b] = level_from_db(20 * std::log10(peak + 1e-9f));
        }
        return out;
    }
};
}
