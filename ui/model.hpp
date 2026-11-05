#pragma once
// Disconnected, in-memory state for the Dusk specimen. Nothing here reaches the earbuds.
// The EQ on screen is the EQ being heard: there is no separate preview mode.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace dusk {
using Curve = std::array<int, 6>; // five bands, then Clear Bass

struct Profile {
    std::wstring name;
    Curve saved{};
    Curve draft{};
    bool factory = false;
    bool curve_known = true;
    unsigned id = 0; // stable across reordering, so an earbud slot follows its EQ
    unsigned preset = 0; // for Sony presets: the earbuds' own preset id
};

// Find your EQ uses Sony's own candidates, read from what Sony's app writes to a WF-C710N
// (Bluetooth log, 5 November 2026). They move only Clear Bass and the top three bands:
// 400 Hz and 1 kHz stay at 0. Curves are {400, 1k, 2.5k, 6.3k, 16k, Clear Bass}.
inline constexpr std::array<Curve, 5> kGuideFirst{{
    {0, 0, 0, 0, 0, 0},     // 1: as it is
    {0, 0, -1, -2, -3, 6},  // 2: more bass, softer top
    {0, 0, 1, 2, 3, 6},     // 3: more bass, brighter top
    {0, 0, 1, 2, 3, -6},    // 4: less bass, brighter top
    {0, 0, -1, -2, -3, -6}, // 5: less bass, softer top
}};
// Round 2: the first pick plus four variations of it, Sony's 6 to 9.
inline constexpr std::array<std::array<Curve, 4>, 5> kGuideSecond{{
    {{{0, 0, 0, 0, 0, 6}, {0, 0, 1, 2, 3, 0}, {0, 0, 0, 0, 0, -6}, {0, 0, -1, -2, -3, 0}}},
    {{{0, 0, -2, -4, -6, 9}, {0, 0, 0, -1, -2, 9}, {0, 0, 0, -1, -2, 3}, {0, 0, -2, -4, -6, 3}}},
    {{{0, 0, 0, 1, 2, 9}, {0, 0, 2, 4, 6, 9}, {0, 0, 2, 4, 6, 3}, {0, 0, 0, 1, 2, 3}}},
    {{{0, 0, 0, 1, 2, -3}, {0, 0, 2, 4, 6, -3}, {0, 0, 2, 4, 6, -9}, {0, 0, 0, 1, 2, -9}}},
    {{{0, 0, -2, -4, -6, -3}, {0, 0, 0, -1, -2, -3}, {0, 0, 0, -1, -2, -9}, {0, 0, -2, -4, -6, -9}}},
}};

// Find your EQ, as Sony's app does it: pick one of five sounds, then one of your pick and four
// variations of it, then hear the result against what was playing before. One option is always
// selected, and each is heard as soon as it is chosen. Nothing in the model changes until Keep.
struct Guide {
    static constexpr int kRounds = 2, kSteps = 3;
    int stage = 0;    // 0 first round, 1 second round, 2 result
    int first = 0;    // 0 to 4: Sony's 1 to 5
    int second = 0;   // 0 is the first pick itself, 1 to 4 are Sony's 6 to 9
    bool before = false; // on the result: play what was playing before
    bool reviewing() const { return stage >= kRounds; }
    Curve curve() const { return stage == 0 || !second ? kGuideFirst[static_cast<size_t>(first)] : kGuideSecond[static_cast<size_t>(first)][static_cast<size_t>(second - 1)]; }
    void choose(int option) {
        if (stage == 0) { first = std::clamp(option, 0, 4); second = 0; }
        else if (stage == 1) second = std::clamp(option, 0, 4);
    }
    void next() { if (stage < kRounds) { ++stage; before = false; } }
    bool back() { if (!stage) return false; --stage; before = false; return true; }
    void retry() { *this = Guide{}; }
};

enum class Noise { Cancelling, Ambient, Off };

inline Profile sample(const wchar_t* name, Curve curve) { return {name, curve, curve, false}; }

struct Model {
    static constexpr size_t kBankSize = 6; // the first six EQs appear on the main screen
    std::vector<Profile> profiles{
        {L"Off", {}, {}, true},
        sample(L"Warm evenings", {3, 1, -1, 1, 2, 3}),
        sample(L"Vocal focus", {-2, 1, 3, 1, -1, 0}),
        sample(L"After hours", {2, -1, -2, -3, -2, 2}),
        sample(L"Open air", {-2, -1, 1, 2, 3, 1}),
        sample(L"Deep listening", {4, -1, -2, 0, 1, 4}),
        sample(L"Quiet desk", {0, 1, 2, 0, -2, 0}),
        sample(L"Late train", {3, 0, -2, 1, 3, 2}),
        sample(L"Wall of sound", {4, 2, -2, 2, 4, 3}),
        sample(L"Podcast", {-3, 1, 4, 2, -1, -2}),
        sample(L"Bright morning", {-1, 0, 1, 3, 4, 0}),
        sample(L"Soft focus", {1, 0, -1, -2, -3, 1}),
        {L"Bright", {}, {}, true, false},
        {L"Excited", {}, {}, true, false},
        {L"Mellow", {}, {}, true, false},
        {L"Relaxed", {}, {}, true, false},
        {L"Vocal", {}, {}, true, false},
        {L"Treble boost", {}, {}, true, false},
        {L"Bass boost", {}, {}, true, false},
        {L"Speech", {}, {}, true, false}};
    size_t selected = 1;
    Noise noise = Noise::Cancelling;
    bool editing = false;
    std::array<bool, 3> cycle{true, true, false};
    std::optional<std::pair<size_t, Profile>> deleted;
    // The earbuds' Custom 1 and Custom 2: EQs kept on the earbuds for the phone or other devices.
    // 0 is empty. Manual is never shown; it always holds whatever EQ is playing.
    std::array<unsigned, 2> kept{};
    unsigned next_id = 0;

    Model() { for (auto& p : profiles) p.id = ++next_id; kept[0] = profiles[7].id; } // Late train starts in Custom 1, as if imported

    std::optional<size_t> find(unsigned id) const {
        for (size_t i = 0; i < profiles.size(); ++i) if (id && profiles[i].id == id) return i;
        return std::nullopt;
    }
    int kept_slot(size_t index) const {
        for (size_t k = 0; k < kept.size(); ++k) if (kept[k] && index < profiles.size() && kept[k] == profiles[index].id) return static_cast<int>(k);
        return -1;
    }
    // Sony presets are never kept: the earbuds already have them. One EQ fills at most one slot.
    bool keep(size_t slot, size_t index) {
        if (slot >= kept.size() || index >= profiles.size() || profiles[index].factory || !profiles[index].curve_known) return false;
        for (auto& id : kept) if (id == profiles[index].id) id = 0;
        kept[slot] = profiles[index].id;
        return true;
    }
    void release(size_t slot) { if (slot < kept.size()) kept[slot] = 0; }

    std::wstring library_label() const {return L"All "+std::to_wstring(profiles.size());}

    Profile& current() { return profiles[selected]; }
    const Profile& current() const { return profiles[selected]; }
    bool dirty() const { return current().draft != current().saved; }
    // Outside the editor the saved version plays. A draft waits until the editor reopens.
    const Curve& heard() const { return editing ? current().draft : current().saved; }

    void select(size_t index) { if (index < profiles.size()) selected = index; }

    // The editor saves as you go. A Sony preset is never changed: the first change makes
    // "<name> (mine)" beside it and continues on that. Returns true when a copy was made.
    bool make_editable() {
        if (!current().factory || !current().curve_known) return false;
        Profile copy = current();
        copy.factory = false;
        std::wstring name = copy.name + L" (mine)";
        for (int number = 2; std::any_of(profiles.begin(), profiles.end(), [&](auto const& p) { return p.name == name; }); ++number) name = copy.name + L" (mine " + std::to_wstring(number) + L")";
        copy.name = name;
        copy.id = ++next_id;
        profiles.insert(profiles.begin() + static_cast<std::ptrdiff_t>(selected) + 1, std::move(copy));
        ++selected;
        return true;
    }
    bool editable() const { return current().curve_known; }
    void set_live(Curve const& curve) {
        if (!editable()) return;
        make_editable();
        for (size_t b = 0; b < curve.size(); ++b) current().draft[b] = std::clamp(curve[b], -10, 10);
        current().saved = current().draft;
    }
    void edit(size_t band, int value) {
        if (band < current().draft.size() && !current().factory) current().draft[band] = std::clamp(value, -10, 10);
    }
    void save() { if (!current().factory) current().saved = current().draft; }
    void revert() { current().draft = current().saved; }
    void flat() { if (!current().factory) current().draft = Curve{}; }
    void duplicate() {
        if(!current().curve_known)return;
        Profile copy = current();
        copy.name += L" copy";
        copy.factory = false;
        copy.saved = copy.draft;
        copy.id = ++next_id;
        profiles.insert(profiles.begin() + static_cast<std::ptrdiff_t>(selected) + 1, std::move(copy));
        ++selected;
    }
    void create() {
        std::wstring name = L"New EQ";
        for (int number = 2; std::any_of(profiles.begin(), profiles.end(), [&](auto const& p) { return p.name == name; }); ++number) name = L"New EQ " + std::to_wstring(number);
        profiles.push_back({name, {}, {}, false});
        profiles.back().id = ++next_id;
        selected = profiles.size() - 1;
    }
    void erase() {
        if (current().factory) return;
        deleted = std::make_pair(selected, current());
        for (auto& id : kept) if (id == current().id) id = 0; // deleting it here also frees its earbud slot
        profiles.erase(profiles.begin() + static_cast<std::ptrdiff_t>(selected));
        selected = std::min(selected, profiles.size() - 1);
    }
    void undo_delete() {
        if (!deleted) return;
        profiles.insert(profiles.begin() + static_cast<std::ptrdiff_t>(deleted->first), deleted->second);
        selected = deleted->first;
        deleted.reset();
    }
    // Reorders the library. The selection stays on the same EQ.
    void move(size_t from, size_t to) {
        if (from >= profiles.size() || to >= profiles.size() || from == to) return;
        const std::wstring name = current().name;
        Profile item = std::move(profiles[from]);
        profiles.erase(profiles.begin() + static_cast<std::ptrdiff_t>(from));
        profiles.insert(profiles.begin() + static_cast<std::ptrdiff_t>(to), std::move(item));
        for (size_t i = 0; i < profiles.size(); ++i) if (profiles[i].name == name) selected = i;
    }
    // The touch control has to move between at least two noise modes.
    bool toggle_cycle(size_t index) {
        if (index >= cycle.size()) return false;
        if (cycle[index] && std::count(cycle.begin(), cycle.end(), true) <= 2) return false;
        cycle[index] = !cycle[index];
        return true;
    }
};
}
