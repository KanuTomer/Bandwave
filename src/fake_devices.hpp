#pragma once
// Development builds only (CMake option BANDWAVE_DEV_FAKES): canned Sony devices, so the Devices
// screen and the widget can be reviewed with several devices without owning them. Started with
// EarbudControls.exe --fake-devices=<scenario>. Never compiled into a release build; no Bluetooth.
#include "controller.hpp"
#include <ctime>

namespace bandwave::fakes {
struct FakeDevice { PairedDevice device; Snapshot state; };

inline Snapshot reading(int left, int right, int box, int noise, uint32_t preset) {
    Snapshot s;
    s.ready = s.fresh = true; s.control = ControlState::Ready; s.windows_connected = true;
    s.noise_supported = s.ambient_supported = s.eq_supported = true;
    s.batteries = {left, right, box}; s.noise = noise; s.ambient = 12; s.voice = 0;
    s.firmware = "1.0.5"; s.checked = std::time(nullptr);
    const std::pair<uint32_t, std::pair<std::vector<int32_t>, int>> curves[]{
        {MDR_EQ_OFF, {{0, 0, 0, 0, 0}, 0}}, {MDR_EQ_BRIGHT, {{0, 1, 3, 5, 6}, 0}}, {MDR_EQ_EXCITED, {{2, 0, 2, 4, 5}, 4}},
        {MDR_EQ_MELLOW, {{1, 0, -2, -3, -4}, 0}}, {MDR_EQ_RELAXED, {{-1, -1, -2, -3, -5}, -2}}, {MDR_EQ_VOCAL, {{-2, 3, 4, 2, 0}, 0}},
        {MDR_EQ_TREBLE, {{0, 0, 2, 5, 7}, 0}}, {MDR_EQ_BASS, {{0, 0, 0, 0, 0}, 7}}, {MDR_EQ_SPEECH, {{-6, 2, 4, 1, -4}, -8}},
        {MDR_EQ_CUSTOM, {{0, 0, 0, 0, 0}, 0}}, {MDR_EQ_USER_1, {{3, 0, -2, 1, 3}, 2}}, {MDR_EQ_USER_2, {{-2, 1, 3, 1, -1}, 0}}};
    for (auto const& [id, curve] : curves) { s.presets.emplace_back(id, preset_name(id)); s.curves[id] = curve; }
    s.preset = preset; s.bands = s.curves[preset].first; s.bass = s.curves[preset].second;
    s.settings = {{"dsee", 1}, {"priority", 1}, {"guidance", 1}, {"guidance_volume", 0}, {"auto_pause", 1}, {"wear_power", 1}, {"shutdown", 1}};
    return s;
}
inline Snapshot away() { Snapshot s; s.batteries = {60, 55, 80}; s.checked = std::time(nullptr) - 7200; s.firmware = "1.0.5"; return s; }
inline FakeDevice device(const char* address, const wchar_t* name, bool connected, Snapshot state) {
    FakeDevice fake{classify(address, name, connected, {}), std::move(state)};
    if (!fake.device.profile && fake.device.windows_name.find(L"buds") != std::wstring::npos) fake.device.profile = profile_by_id("wf-c710n"); // a renamed pair
    fake.device.sony = true;
    return fake;
}
inline std::vector<FakeDevice> scenario(std::string_view name) {
    std::vector<FakeDevice> list;
    if (name == "one") list.push_back(device("F0:00:00:00:00:01", L"WF-C710N", true, reading(82, 78, 100, 1, MDR_EQ_OFF)));
    else if (name == "two-same") {
        list.push_back(device("F0:00:00:00:00:01", L"WF-C710N", true, reading(82, 78, 100, 1, MDR_EQ_OFF)));
        list.push_back(device("F0:00:00:00:00:02", L"Kanu's buds", true, reading(40, 45, 10, 2, MDR_EQ_BRIGHT)));
    } else if (name == "four") {
        list.push_back(device("F0:00:00:00:00:01", L"WF-C710N", true, reading(82, 78, 100, 1, MDR_EQ_OFF)));
        list.push_back(device("F0:00:00:00:00:02", L"Kanu's buds", false, away()));
        list.push_back(device("F0:00:00:00:00:03", L"WH-1000XM5", true, {}));
        list.push_back(device("F0:00:00:00:00:04", L"WF-1000XM4", false, {}));
    } else if (name == "in-use") {
        Snapshot held; held.windows_connected = true; held.control = ControlState::InUse; held.batteries = {70, 70, 90}; held.checked = std::time(nullptr) - 600;
        list.push_back(device("F0:00:00:00:00:01", L"WF-C710N", true, std::move(held)));
    }
    return list; // "none": nothing paired
}
}
