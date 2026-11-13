#pragma once
// Where each device's state lives. Every device has its own folder, so its EQ library and
// last-known values never mix with another's:
//
//   <data folder>\devices.json                      which device is focused, and the ones seen
//   <data folder>\devices\AABBCCDDEEFF\state.json   that device's last-known values
//   <data folder>\devices\AABBCCDDEEFF\library.json that device's EQs
//
// Before 0.10.0 there was one device: state.json and library.json in the data folder itself.
// migrate_legacy() copies them into that device's folder once and leaves the originals alone.
#include "library.hpp"

namespace bandwave {
namespace json = winrt::Windows::Data::Json;

struct KnownDevice {
    std::string address;
    std::string model;   // profile id, once known; kept so a device renamed in Windows keeps its profile
    std::wstring name;   // as Windows last showed it
    bool operator==(KnownDevice const&) const = default;
};
struct DeviceIndex {
    std::string focused; // the device open, or last open, in the app; widgets follow it
    bool logging = false;
    int release = -1; // "Let the phone app connect", for every device: 1 on, 0 off, -1 not set yet
    std::vector<KnownDevice> known;
    bool operator==(DeviceIndex const&) const = default;

    KnownDevice* find(std::string const& address) {
        for (auto& d : known) if (d.address == address) return &d;
        return nullptr;
    }
    // Returns true when something changed.
    bool remember(std::string const& address, std::string const& model, std::wstring const& name) {
        if (auto* d = find(address)) {
            const bool changed = (!model.empty() && d->model != model) || d->name != name;
            if (!model.empty()) d->model = model;
            d->name = name;
            return changed;
        }
        known.push_back({address, model, name});
        return true;
    }
};

inline bool valid_address(std::string_view address) {
    if (address.size() != 17) return false;
    for (size_t i = 0; i < 17; ++i) {
        const char c = address[i];
        if (i % 3 == 2) { if (c != ':') return false; }
        else if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}
inline std::filesystem::path device_folder(std::filesystem::path const& root, std::string_view address) {
    std::wstring name;
    for (char c : address) if (c != ':') name += static_cast<wchar_t>(c);
    return root / L"devices" / name;
}
inline std::filesystem::path index_path(std::filesystem::path const& root) { return root / L"devices.json"; }

inline std::wstring to_text(DeviceIndex const& index) {
    json::JsonObject root;
    root.SetNamedValue(L"version", json::JsonValue::CreateNumberValue(1));
    root.SetNamedValue(L"focused", json::JsonValue::CreateStringValue(winrt::to_hstring(index.focused)));
    root.SetNamedValue(L"logging", json::JsonValue::CreateBooleanValue(index.logging));
    if (index.release >= 0) root.SetNamedValue(L"releaseWhenHidden", json::JsonValue::CreateBooleanValue(index.release == 1));
    json::JsonArray known;
    for (auto const& d : index.known) {
        json::JsonObject item;
        item.SetNamedValue(L"address", json::JsonValue::CreateStringValue(winrt::to_hstring(d.address)));
        item.SetNamedValue(L"model", json::JsonValue::CreateStringValue(winrt::to_hstring(d.model)));
        item.SetNamedValue(L"name", json::JsonValue::CreateStringValue(d.name));
        known.Append(item);
    }
    root.SetNamedValue(L"known", known);
    return std::wstring(root.Stringify());
}
inline std::optional<DeviceIndex> index_from_text(std::wstring_view text) {
    json::JsonObject root{nullptr};
    if (text.empty() || !json::JsonObject::TryParse(text, root)) return std::nullopt;
    try {
        DeviceIndex index;
        index.focused = winrt::to_string(root.GetNamedString(L"focused", L""));
        if (!index.focused.empty() && !valid_address(index.focused)) index.focused.clear();
        index.logging = root.GetNamedBoolean(L"logging", false);
        if (root.HasKey(L"releaseWhenHidden")) index.release = root.GetNamedBoolean(L"releaseWhenHidden", false) ? 1 : 0;
        if (root.HasKey(L"known")) {
            auto known = root.GetNamedArray(L"known");
            if (known.Size() > 64) return std::nullopt;
            for (auto value : known) {
                auto item = value.GetObject();
                KnownDevice d{winrt::to_string(item.GetNamedString(L"address", L"")), winrt::to_string(item.GetNamedString(L"model", L"")), std::wstring(item.GetNamedString(L"name", L""))};
                if (!valid_address(d.address) || d.model.size() > 32 || d.name.size() > 248 || index.find(d.address)) continue;
                index.known.push_back(std::move(d));
            }
        }
        return index;
    } catch (winrt::hresult_error const&) { return std::nullopt; }
}
inline bool save_index(std::filesystem::path const& root, DeviceIndex const& index) { return earbud::write_atomic(index_path(root), to_text(index)); }
// A missing index is an empty one. An unreadable one is set aside, never overwritten in place.
inline DeviceIndex load_index(std::filesystem::path const& root) {
    std::error_code ec;
    const auto path = index_path(root);
    if (!std::filesystem::exists(path, ec)) return {};
    if (auto index = index_from_text(earbud::read_text(path))) return *index;
    const auto stamp = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::filesystem::rename(path, root / (L"devices.unreadable-" + std::to_wstring(stamp) + L".json"), ec);
    return {};
}

// Once, on the first start of 0.10.0: the single device's files are copied into its own folder.
// The originals stay where they are. With no remembered address the library waits in
// devices\_unassigned and goes to the first device of that model to appear (adopt_unassigned).
inline void migrate_legacy(std::filesystem::path const& root) {
    std::error_code ec;
    if (std::filesystem::exists(index_path(root), ec)) return;
    const auto state = root / L"state.json", library = root / L"library.json";
    DeviceIndex index;
    std::string address;
    json::JsonObject old{nullptr};
    if (std::filesystem::exists(state, ec) && json::JsonObject::TryParse(earbud::read_text(state, 65536), old)) {
        address = winrt::to_string(old.GetNamedString(L"address", L""));
        index.logging = old.GetNamedBoolean(L"logging", false);
        index.release = old.GetNamedBoolean(L"release_when_hidden", false) ? 1 : 0;
    }
    if (valid_address(address)) {
        const auto folder = device_folder(root, address);
        std::filesystem::create_directories(folder, ec);
        std::filesystem::copy_file(state, folder / L"state.json", std::filesystem::copy_options::skip_existing, ec);
        if (std::filesystem::exists(library, ec)) std::filesystem::copy_file(library, folder / L"library.json", std::filesystem::copy_options::skip_existing, ec);
        index.focused = address;
        index.known.push_back({address, "wf-c710n", L"WF-C710N"}); // the only model before 0.10.0
    } else if (std::filesystem::exists(library, ec)) {
        const auto folder = root / L"devices" / L"_unassigned";
        std::filesystem::create_directories(folder, ec);
        std::filesystem::copy_file(library, folder / L"library.json", std::filesystem::copy_options::skip_existing, ec);
    }
    save_index(root, index);
}
// A library saved before its device's address was known goes to the first WF-C710N seen.
inline void adopt_unassigned(std::filesystem::path const& root, std::string_view address, std::string_view model) {
    std::error_code ec;
    const auto waiting = root / L"devices" / L"_unassigned" / L"library.json";
    if (model != "wf-c710n" || !std::filesystem::exists(waiting, ec)) return;
    const auto folder = device_folder(root, address);
    std::filesystem::create_directories(folder, ec);
    if (!std::filesystem::exists(folder / L"library.json", ec)) std::filesystem::rename(waiting, folder / L"library.json", ec);
}
}
