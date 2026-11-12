#pragma once
// What the app needs to know about a Sony model that the device itself can't tell it. One
// ModelProfile per supported model, compiled in (sony_models.hpp). Everything else (noise modes,
// EQ presets, settings and their options, batteries) is read from the device.
// No Windows headers here: the owner, the app and the tests all include this.
#include <array>
#include <span>
#include <string_view>

namespace bandwave {
enum class FormFactor { Buds, Headphones }; // Buds report left, right and case batteries
enum class Transport { ClassicV2, ClassicV1, Ble };

// Find your EQ, as Sony's app does it for a model: five first choices, then four variations of
// each. Curves are five bands, then Clear Bass.
using Curve = std::array<int, 6>;
struct FindEqTable {
    std::array<Curve, 5> first;
    std::array<std::array<Curve, 4>, 5> second;
};

struct ModelProfile {
    std::string_view id;            // stable key, lowercase; stored on disk ("wf-c710n")
    std::wstring_view display_name; // "WF-C710N"
    std::wstring_view noun;         // "earbuds" or "headphones", for device copy
    std::span<const std::wstring_view> windows_names; // the names Windows gives it, exact, any case
    std::string_view reported_model; // MDR_TEXT_MODEL_NAME must equal this before any write
    FormFactor form;
    Transport transport;
    std::wstring_view art_folder;   // under the app's Assets folder
    std::string_view widget_art;    // the widget's artwork key
    std::array<std::wstring_view, 5> band_labels;
    int band_min, band_max;
    bool clear_bass;
    const FindEqTable* find_eq;     // nullptr: Find your EQ isn't offered
    // Features checked on real hardware. A control is shown only when the device advertises it
    // and its key is here. "general_" covers every general_<index> setting.
    std::span<const std::string_view> verified;

    constexpr bool is_verified(std::string_view feature) const {
        for (auto key : verified) {
            if (key == feature) return true;
            if (key.ends_with('_') && feature.starts_with(key)) return true;
        }
        return false;
    }
};

constexpr wchar_t lower(wchar_t c) { return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c + (L'a' - L'A')) : c; }
constexpr bool same_name(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (lower(a[i]) != lower(b[i])) return false;
    return true;
}
}
