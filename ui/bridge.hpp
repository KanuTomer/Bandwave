#pragma once
// Converts between the app's editing model (model.hpp) and the shared library.json form
// (src/library.hpp) that the owner process saves and the widget reads.
#include "model.hpp"
#include "../src/library.hpp"

namespace dusk {
inline earbud::Library to_library(Model const& model, bool imported) {
    earbud::Library library;
    library.imported = imported;
    unsigned highest = 0;
    for (auto const& p : model.profiles) {
        earbud::Profile out;
        out.id = p.id;
        out.name = p.name.substr(0, earbud::kMaxName);
        if (out.name.empty()) out.name = L"Untitled EQ";
        out.kind = p.factory ? earbud::ProfileKind::Sony : earbud::ProfileKind::Personal;
        out.preset = p.preset;
        out.curve_known = p.curve_known;
        for (size_t b = 0; b < 5; ++b) out.bands[b] = std::clamp(p.saved[b], -10, 10);
        out.clear_bass = std::clamp(p.saved[5], -10, 10);
        highest = std::max(highest, p.id);
        library.profiles.push_back(std::move(out));
    }
    library.kept = {model.kept[0], model.kept[1]};
    library.next_id = std::max<unsigned>(model.next_id, highest) + 1;
    return library;
}

// Replaces the model's EQs with the library's, keeping the same EQ selected when it still exists.
inline void load_library(Model& model, earbud::Library const& library) {
    const unsigned selected_id = model.selected < model.profiles.size() ? model.profiles[model.selected].id : 0;
    model.profiles.clear();
    for (auto const& p : library.profiles) {
        Profile in;
        in.name = p.name;
        in.factory = p.kind == earbud::ProfileKind::Sony;
        in.preset = p.preset;
        in.curve_known = p.curve_known;
        in.id = p.id;
        for (size_t b = 0; b < 5; ++b) in.saved[b] = p.bands[b];
        in.saved[5] = p.clear_bass;
        in.draft = in.saved;
        model.profiles.push_back(std::move(in));
    }
    model.kept = {library.kept[0], library.kept[1]};
    model.next_id = library.next_id ? library.next_id - 1 : 0;
    model.deleted.reset();
    model.selected = 0;
    if (auto found = model.find(selected_id)) model.selected = *found;
}
}
