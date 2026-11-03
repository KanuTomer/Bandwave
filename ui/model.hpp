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

inline Curve guided_curve(std::array<int,3> choices,double strength){
    strength=std::clamp(strength,0.0,1.0);for(auto& choice:choices)choice=std::clamp(choice,0,2)-1;
    auto gain=[&](int choice,int scale){return static_cast<int>(std::lround(choice*scale*strength));};
    // Five bands only. Clear Bass is a separate earbud setting, not the low end of the EQ, so the guide leaves it at 0.
    return {gain(choices[0],4),0,gain(choices[1],4),gain(choices[2],3),gain(choices[2],4),0};
}

// Find your EQ: three rounds, each a choice heard between two versions. A is 0, B is 2, and
// "can't tell" or no choice yet is 1. Nothing in the model changes until the result is kept.
struct Guide {
    static constexpr int kRounds = 3;
    int stage = 0;
    std::array<int, 3> choices{1, 1, 1};
    std::array<bool, 3> answered{};
    double strength = .75;
    bool reviewing() const { return stage >= kRounds; }
    Curve curve() const { return guided_curve(choices, reviewing() ? strength : 1); }
    void choose(int option) { if (!reviewing()) { choices[static_cast<size_t>(stage)] = std::clamp(option, 0, 2); answered[static_cast<size_t>(stage)] = true; } }
    bool can_advance() const { return reviewing() || answered[static_cast<size_t>(stage)]; }
    void next() { if (can_advance()) stage = std::min(stage + 1, kRounds); }
    bool back() { if (!stage) return false; --stage; return true; }
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
