#include "library.hpp"
#include <cstdio>
#include <cstdlib>
#define CHECK(condition) do { if (!(condition)) { std::printf("FAILED line %d: %s\n", __LINE__, #condition); std::exit(1); } } while (0)
using namespace earbud;

static Library sample() {
    Library library;
    library.profiles.push_back({1, L"Off", ProfileKind::Sony, 0, true, {}, 0});
    library.profiles.push_back({2, L"Warm evenings", ProfileKind::Personal, 0, true, {3, 1, -1, 1, 2}, 3});
    library.profiles.push_back({3, L"Bass boost", ProfileKind::Sony, 7, false, {}, 0});
    library.kept = {2, 0};
    library.next_id = 4;
    return library;
}

int main() {
    winrt::init_apartment();
    // Round trip keeps everything, including Sony preset ids and unread curves.
    const auto original = sample();
    auto text = std::wstring(to_json(original).Stringify());
    auto back = from_json(text);
    CHECK(back && *back == original);
    CHECK((back->main_screen() == std::vector<uint32_t>{1, 2, 3}));

    // Anything out of range is rejected whole, never clamped or half-loaded.
    auto bad = original; bad.profiles[1].bands[0] = 11;
    CHECK(!from_json(std::wstring(to_json(bad).Stringify())));
    bad = original; bad.profiles[1].clear_bass = -11;
    CHECK(!from_json(std::wstring(to_json(bad).Stringify())));
    bad = original; bad.kept = {3, 0};                 // a Sony preset can't be kept on the earbuds
    CHECK(!from_json(std::wstring(to_json(bad).Stringify())));
    bad = original; bad.kept = {2, 2};                 // one EQ fills at most one slot
    CHECK(!from_json(std::wstring(to_json(bad).Stringify())));
    bad = original; bad.profiles[2].id = 2;            // ids are unique
    CHECK(!from_json(std::wstring(to_json(bad).Stringify())));
    bad = original; bad.next_id = 3;                   // next_id stays ahead of every id
    CHECK(!from_json(std::wstring(to_json(bad).Stringify())));
    bad = original; bad.profiles[1].name = std::wstring(kMaxName + 1, L'x');
    CHECK(!from_json(std::wstring(to_json(bad).Stringify())));
    CHECK(!from_json(L"{\"version\":2}"));
    CHECK(!from_json(L"not json"));

    // Storage: atomic save, reload, and an unreadable file kept aside rather than deleted.
    const auto folder = std::filesystem::temp_directory_path() / (L"earbud-library-test-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
    bool recovered = true;
    CHECK(load_library(folder, &recovered).profiles.empty() && !recovered);
    CHECK(save_library(folder, original));
    CHECK(load_library(folder, &recovered) == original && !recovered);
    CHECK(!std::filesystem::exists(folder / L"library.json.tmp"));
    CHECK(write_atomic(folder / L"library.json", L"{ broken"));
    CHECK(load_library(folder, &recovered).profiles.empty() && recovered);
    size_t kept_aside = 0;
    for (auto const& entry : std::filesystem::directory_iterator(folder))
        if (entry.path().filename().wstring().starts_with(L"library.unreadable-")) ++kept_aside;
    CHECK(kept_aside == 1);

    // 0.4.x state moves across once and the original stays.
    const auto old_state = folder / L"old";
    std::filesystem::create_directories(old_state);
    CHECK(write_atomic(old_state / L"state.json", L"{\"noise\":1}"));
    migrate_package_state(folder / L"new", old_state);
    CHECK(read_text(folder / L"new" / L"state.json") == L"{\"noise\":1}" && std::filesystem::exists(old_state / L"state.json"));
    CHECK(write_atomic(old_state / L"state.json", L"{\"noise\":2}"));
    migrate_package_state(folder / L"new", old_state);   // never overwrites what is already there
    CHECK(read_text(folder / L"new" / L"state.json") == L"{\"noise\":1}");
    std::filesystem::remove_all(folder, ec);
    std::printf("library tests passed\n");
    return 0;
}
