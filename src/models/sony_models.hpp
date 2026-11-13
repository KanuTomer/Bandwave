#pragma once
// The Sony models Bandwave supports. Adding one: add its tables and an entry to kProfiles, its
// art folder under ui/Assets, and its widget art. Nothing else in the app names a model.
#include "model_profile.hpp"

namespace bandwave {
namespace models {
// ---- WF-C710N ------------------------------------------------------------------------------
inline constexpr std::wstring_view kWfC710nNames[]{L"WF-C710N"};
// Sony's own Find your EQ candidates, read from what Sony's app writes to a WF-C710N (Bluetooth
// log, 5 November 2026). They move only Clear Bass and the top three bands: 400 Hz and 1 kHz stay
// at 0. Curves are {400, 1k, 2.5k, 6.3k, 16k, Clear Bass}.
inline constexpr FindEqTable kWfC710nFindEq{
    {{
        {0, 0, 0, 0, 0, 0},     // 1: as it is
        {0, 0, -1, -2, -3, 6},  // 2: more bass, softer top
        {0, 0, 1, 2, 3, 6},     // 3: more bass, brighter top
        {0, 0, 1, 2, 3, -6},    // 4: less bass, brighter top
        {0, 0, -1, -2, -3, -6}, // 5: less bass, softer top
    }},
    // Round 2: the first pick plus four variations of it, Sony's 6 to 9.
    {{
        {{{0, 0, 0, 0, 0, 6}, {0, 0, 1, 2, 3, 0}, {0, 0, 0, 0, 0, -6}, {0, 0, -1, -2, -3, 0}}},
        {{{0, 0, -2, -4, -6, 9}, {0, 0, 0, -1, -2, 9}, {0, 0, 0, -1, -2, 3}, {0, 0, -2, -4, -6, 3}}},
        {{{0, 0, 0, 1, 2, 9}, {0, 0, 2, 4, 6, 9}, {0, 0, 2, 4, 6, 3}, {0, 0, 0, 1, 2, 3}}},
        {{{0, 0, 0, 1, 2, -3}, {0, 0, 2, 4, 6, -3}, {0, 0, 2, 4, 6, -9}, {0, 0, 0, 1, 2, -9}}},
        {{{0, 0, -2, -4, -6, -3}, {0, 0, 0, -1, -2, -3}, {0, 0, 0, -1, -2, -9}, {0, 0, -2, -4, -6, -9}}},
    }}};
// Everything the app shows today was checked on a WF-C710N.
inline constexpr std::string_view kWfC710nVerified[]{
    "noise", "ambient", "voice", "eq", "clear_bass", "slots", "find_eq",
    "dsee", "priority", "guidance", "guidance_volume", "auto_pause", "wear_power", "shutdown",
    "cycle", "touch_left", "touch_right", "general_"};

inline constexpr ModelProfile kProfiles[]{
    {"wf-c710n", L"WF-C710N", L"earbuds", kWfC710nNames, "WF-C710N", FormFactor::Buds, Transport::ClassicV2,
     L"wf-c710n", "wf-c710n", {L"400", L"1k", L"2.5k", L"6.3k", L"16k"}, -10, 10, true, &kWfC710nFindEq, kWfC710nVerified},
};
}

constexpr std::span<const ModelProfile> all_profiles() { return models::kProfiles; }
constexpr const ModelProfile* profile_by_id(std::string_view id) {
    for (auto& p : models::kProfiles) if (p.id == id) return &p;
    return nullptr;
}
constexpr const ModelProfile* profile_for_windows_name(std::wstring_view name) {
    for (auto& p : models::kProfiles) for (auto known : p.windows_names) if (same_name(known, name)) return &p;
    return nullptr;
}
// What the device calls itself over Sony's protocol; survives a rename in Windows.
constexpr const ModelProfile* profile_for_reported_model(std::string_view model) {
    for (auto& p : models::kProfiles) if (p.reported_model == model) return &p;
    return nullptr;
}
}
