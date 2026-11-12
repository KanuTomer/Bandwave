#pragma once
// The Sony devices Windows has paired. Windows owns pairing, connecting and forgetting; this only
// reads what it knows and never scans. A device is Sony's when it offers Sony's control service,
// or (when Windows hasn't recorded its services) when its name looks like a Sony model.
#include <windows.h>
#include <bluetoothapis.h>
#include <cstdio>
#include <span>
#include <string>
#include <vector>
#include "models/sony_models.hpp"

namespace bandwave {
// MDR_SERVICE_UUID_V2, _V1 and _BLE (libmdr's Base.h), as GUIDs.
inline constexpr GUID kSonyServices[]{
    {0x956C7B26, 0xD49A, 0x4BA8, {0xB0, 0x3F, 0xB1, 0x7D, 0x39, 0x3C, 0xB6, 0xE2}},
    {0x96CC203E, 0x5068, 0x46AD, {0xB3, 0x2D, 0xE3, 0x16, 0xF5, 0xE0, 0x69, 0xBA}},
    {0x5B833E20, 0x6BC7, 0x4802, {0x8E, 0x9A, 0x72, 0x3C, 0xEC, 0xA4, 0xBD, 0x8F}}};

struct PairedDevice {
    std::string address;        // "AA:BB:CC:DD:EE:FF"
    std::wstring windows_name;  // as Windows shows it; the user may have renamed it
    bool connected{};
    bool sony{};
    const ModelProfile* profile{}; // nullptr: Sony's, but not supported yet
};

// Windows lists a Bluetooth LE identity as "LE_<name>".
inline std::wstring_view model_part(std::wstring_view name) {
    return name.size() > 3 && same_name(name.substr(0, 3), L"LE_") ? name.substr(3) : name;
}
inline bool sony_name(std::wstring_view name) {
    name = model_part(name);
    for (std::wstring_view prefix : {L"WF-", L"WH-", L"WI-", L"LinkBuds", L"ULT "})
        if (name.size() >= prefix.size() && same_name(name.substr(0, prefix.size()), prefix)) return true;
    return false;
}
inline bool sony_service(std::span<const GUID> services) {
    for (auto const& service : services) for (auto const& known : kSonyServices) if (IsEqualGUID(service, known)) return true;
    return false;
}
// Decides whose a paired device is, from what Windows gives. No Bluetooth calls.
inline PairedDevice classify(std::string address, std::wstring name, bool connected, std::span<const GUID> services) {
    PairedDevice device{std::move(address), std::move(name), connected};
    device.profile = profile_for_windows_name(model_part(device.windows_name));
    device.sony = device.profile || sony_service(services) || sony_name(device.windows_name);
    return device;
}
inline std::string format_address(const BLUETOOTH_ADDRESS& value) {
    const auto* b = value.rgBytes;
    char text[18]{};
    snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X", b[5], b[4], b[3], b[2], b[1], b[0]);
    return text;
}
// Adds a device seen on another radio, or under a second identity, to the list once.
inline void merge(std::vector<PairedDevice>& list, PairedDevice device) {
    for (auto& known : list) if (known.address == device.address) {
        known.connected |= device.connected;
        if (!known.profile) known.profile = device.profile;
        return;
    }
    list.push_back(std::move(device));
}
// Every paired Sony device, in the order Windows returns them. Empty when there is no radio.
inline std::vector<PairedDevice> list_paired_sony() {
    std::vector<PairedDevice> list;
    BLUETOOTH_FIND_RADIO_PARAMS params{sizeof(params)};
    HANDLE radio{};
    auto radios = BluetoothFindFirstRadio(&params, &radio);
    if (!radios) return list;
    do {
        BLUETOOTH_DEVICE_SEARCH_PARAMS search{};
        search.dwSize = sizeof(search);
        search.fReturnAuthenticated = TRUE;
        search.fReturnRemembered = TRUE;
        search.fReturnConnected = TRUE;
        search.fIssueInquiry = FALSE;
        search.hRadio = radio;
        BLUETOOTH_DEVICE_INFO info{sizeof(info)};
        if (auto devices = BluetoothFindFirstDevice(&search, &info)) {
            do {
                GUID services[64]{};
                DWORD count = 64;
                if (BluetoothEnumerateInstalledServices(radio, &info, &count, services) != ERROR_SUCCESS) count = 0;
                auto device = classify(format_address(info.Address), info.szName, info.fConnected != FALSE, std::span<const GUID>(services, count < 64 ? count : 64));
                if (device.sony) merge(list, std::move(device));
            } while (BluetoothFindNextDevice(devices, &info));
            BluetoothFindDeviceClose(devices);
        }
        CloseHandle(radio);
    } while (BluetoothFindNextRadio(radios, &radio));
    BluetoothFindRadioClose(radios);
    return list;
}
}
