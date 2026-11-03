#pragma once
// The EQ library shared by the app (EarbudControls.UI.exe) and the owner/widget process
// (EarbudControls.exe), stored in library.json in the data folder. The owner is the only writer.
//
// Sony presets are referenced by the earbuds' own preset id; their curves are cached once read
// from the device. Personal EQs hold five bands and Clear Bass (a separate earbud setting).
// The first six profiles in order are the ones on the app's main screen and in the widget.
// kept[0] and kept[1] are the ids held in the earbuds' Custom 1 and Custom 2 (0 is empty).
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <shlobj.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace earbud {
namespace json = winrt::Windows::Data::Json;

inline constexpr int kLibraryVersion = 1;
inline constexpr size_t kMainScreen = 6, kMaxProfiles = 200, kMaxName = 32;

enum class ProfileKind { Personal, Sony };

struct Profile {
    uint32_t id = 0;
    std::wstring name;
    ProfileKind kind = ProfileKind::Personal;
    uint32_t preset = 0;           // the earbuds' preset id, for Sony presets
    bool curve_known = true;       // false until a Sony preset's curve has been read from the device
    std::array<int, 5> bands{};    // 400 Hz, 1 kHz, 2.5 kHz, 6.3 kHz, 16 kHz; -10..10
    int clear_bass = 0;            // -10..10
    bool operator==(Profile const&) const = default;
};

struct Library {
    std::vector<Profile> profiles;
    std::array<uint32_t, 2> kept{};
    uint32_t next_id = 1;
    bool imported = false;         // the earbuds' Manual/Custom 1/Custom 2 were imported once
    bool operator==(Library const&) const = default;

    Profile const* find(uint32_t id) const {
        for (auto const& p : profiles) if (id && p.id == id) return &p;
        return nullptr;
    }
    std::vector<uint32_t> main_screen() const {
        std::vector<uint32_t> ids;
        for (size_t i = 0; i < profiles.size() && i < kMainScreen; ++i) ids.push_back(profiles[i].id);
        return ids;
    }
};

// ---------- JSON ----------
inline json::JsonObject to_json(Library const& library) {
    json::JsonObject root;
    root.SetNamedValue(L"version", json::JsonValue::CreateNumberValue(kLibraryVersion));
    root.SetNamedValue(L"next_id", json::JsonValue::CreateNumberValue(library.next_id));
    root.SetNamedValue(L"imported", json::JsonValue::CreateBooleanValue(library.imported));
    json::JsonArray kept;
    for (auto id : library.kept) kept.Append(json::JsonValue::CreateNumberValue(id));
    root.SetNamedValue(L"kept", kept);
    json::JsonArray profiles;
    for (auto const& p : library.profiles) {
        json::JsonObject item;
        item.SetNamedValue(L"id", json::JsonValue::CreateNumberValue(p.id));
        item.SetNamedValue(L"name", json::JsonValue::CreateStringValue(p.name));
        item.SetNamedValue(L"kind", json::JsonValue::CreateStringValue(p.kind == ProfileKind::Sony ? L"sony" : L"personal"));
        if (p.kind == ProfileKind::Sony) item.SetNamedValue(L"preset", json::JsonValue::CreateNumberValue(p.preset));
        item.SetNamedValue(L"curve_known", json::JsonValue::CreateBooleanValue(p.curve_known));
        json::JsonArray bands;
        for (int b : p.bands) bands.Append(json::JsonValue::CreateNumberValue(b));
        item.SetNamedValue(L"bands", bands);
        item.SetNamedValue(L"clear_bass", json::JsonValue::CreateNumberValue(p.clear_bass));
        profiles.Append(item);
    }
    root.SetNamedValue(L"profiles", profiles);
    return root;
}

// Strict: anything out of range or malformed rejects the whole file rather than guessing.
inline std::optional<Library> from_json(std::wstring_view text) {
    json::JsonObject root{nullptr};
    if (text.size() > 1'000'000 || !json::JsonObject::TryParse(winrt::hstring(text), root)) return std::nullopt;
    try {
        if (static_cast<int>(root.GetNamedNumber(L"version")) != kLibraryVersion) return std::nullopt;
        Library library;
        library.next_id = static_cast<uint32_t>(root.GetNamedNumber(L"next_id"));
        library.imported = root.GetNamedBoolean(L"imported", false);
        auto profiles = root.GetNamedArray(L"profiles");
        if (profiles.Size() > kMaxProfiles) return std::nullopt;
        const auto whole = [](double v, double lo, double hi) { return v >= lo && v <= hi && v == static_cast<double>(static_cast<long long>(v)); };
        for (uint32_t index = 0; index < profiles.Size(); ++index) {
            auto item = profiles.GetObjectAt(index); // not GetObject: wingdi's macro may have renamed it
            Profile p;
            const double id = item.GetNamedNumber(L"id");
            if (!whole(id, 1, 4'000'000'000.0)) return std::nullopt;
            p.id = static_cast<uint32_t>(id);
            p.name = item.GetNamedString(L"name").c_str();
            if (p.name.empty() || p.name.size() > kMaxName) return std::nullopt;
            const std::wstring kind{item.GetNamedString(L"kind")};
            if (kind != L"sony" && kind != L"personal") return std::nullopt;
            p.kind = kind == L"sony" ? ProfileKind::Sony : ProfileKind::Personal;
            if (p.kind == ProfileKind::Sony) { const double preset = item.GetNamedNumber(L"preset"); if (!whole(preset, 0, 255)) return std::nullopt; p.preset = static_cast<uint32_t>(preset); }
            p.curve_known = item.GetNamedBoolean(L"curve_known", true);
            auto bands = item.GetNamedArray(L"bands");
            if (bands.Size() != 5) return std::nullopt;
            for (uint32_t b = 0; b < 5; ++b) { const double v = bands.GetNumberAt(b); if (!whole(v, -10, 10)) return std::nullopt; p.bands[b] = static_cast<int>(v); }
            const double bass = item.GetNamedNumber(L"clear_bass");
            if (!whole(bass, -10, 10)) return std::nullopt;
            p.clear_bass = static_cast<int>(bass);
            if (library.find(p.id) || p.id >= library.next_id) return std::nullopt;
            library.profiles.push_back(std::move(p));
        }
        auto kept = root.GetNamedArray(L"kept");
        if (kept.Size() != 2) return std::nullopt;
        for (uint32_t k = 0; k < 2; ++k) {
            const double id = kept.GetNumberAt(k);
            if (!whole(id, 0, 4'000'000'000.0)) return std::nullopt;
            library.kept[k] = static_cast<uint32_t>(id);
            auto const* held = library.find(library.kept[k]);
            if (library.kept[k] && (!held || held->kind != ProfileKind::Personal)) return std::nullopt;
        }
        if (library.kept[0] && library.kept[0] == library.kept[1]) return std::nullopt;
        return library;
    } catch (winrt::hresult_error const&) { return std::nullopt; }
}

// ---------- Storage ----------
// %LOCALAPPDATA%\Earbud Controls. The package disables write virtualization, so this folder is
// real and outlives a package uninstall: the uninstaller asks before deleting it.
inline std::filesystem::path data_folder() {
    PWSTR raw{};
    winrt::check_hresult(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw));
    std::filesystem::path path = std::filesystem::path(raw) / L"Earbud Controls";
    CoTaskMemFree(raw);
    return path;
}

inline std::wstring read_text(std::filesystem::path const& path, uintmax_t limit = 1'000'000) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || std::filesystem::file_size(path, ec) > limit) return {};
    std::ifstream input(path, std::ios::binary);
    std::string bytes{std::istreambuf_iterator<char>(input), {}};
    return std::wstring(winrt::to_hstring(bytes));
}

// Write a temporary file beside the target, then replace it, so a crash never leaves half a file.
inline bool write_atomic(std::filesystem::path const& path, std::wstring_view text) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    auto temp = path;
    temp += L".tmp";
    {
        std::ofstream output(temp, std::ios::binary | std::ios::trunc);
        const std::string bytes = winrt::to_string(text);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!output.flush()) return false;
    }
    return MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

inline bool save_library(std::filesystem::path const& folder, Library const& library) {
    return write_atomic(folder / L"library.json", to_json(library).Stringify());
}

// A missing file starts an empty library. A file that can't be read is kept, renamed with a
// timestamp, and never deleted, so nothing the listener made is lost to a bad write or edit.
inline Library load_library(std::filesystem::path const& folder, bool* recovered = nullptr) {
    const auto path = folder / L"library.json";
    if (recovered) *recovered = false;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return {};
    if (auto library = from_json(read_text(path))) return *library;
    const auto stamp = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::filesystem::rename(path, folder / (L"library.unreadable-" + std::to_wstring(stamp) + L".json"), ec);
    if (recovered) *recovered = true;
    return {};
}

// 0.4.x kept state.json in the package's own folder, which Windows deletes on uninstall. Copy it
// across once; the original is left where it was.
inline void migrate_package_state(std::filesystem::path const& folder, std::filesystem::path const& package_local_state) {
    std::error_code ec;
    const auto target = folder / L"state.json", source = package_local_state / L"state.json";
    if (std::filesystem::exists(target, ec) || !std::filesystem::exists(source, ec)) return;
    std::filesystem::create_directories(folder, ec);
    std::filesystem::copy_file(source, target, ec);
}
}
