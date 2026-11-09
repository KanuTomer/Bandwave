#pragma once
#include <windows.h>
#include <bluetoothapis.h>
#include <sddl.h>
#include <winrt/base.h>
#include <mdr-c/Headphones.h>
#include <mdr-bt/ConnectionWindows.h>
#include "event_transport.hpp"
#include <chrono>
#include <array>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <memory>

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

static std::string quoted(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 0x20) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c);
        else out << c;
    }
    out << '"';
    return out.str();
}

static void check(MDRResult result, const char* operation) {
    if (result != MDR_RESULT_OK)
        throw std::runtime_error(std::string(operation) + ": " + mdrResultString(result));
}

// Paired earbuds that Windows has not connected: wait for the link instead of retrying.
struct NotConnected : std::runtime_error { using std::runtime_error::runtime_error; };
struct Device { std::string address; bool connected; };
static Device find_earbuds() {
    BLUETOOTH_FIND_RADIO_PARAMS params{sizeof(params)};
    HANDLE radio{};
    auto radios = BluetoothFindFirstRadio(&params, &radio);
    if (!radios) throw std::runtime_error("No available Bluetooth radio. Check Windows Bluetooth settings.");
    std::vector<Device> matches;
    do {
        BLUETOOTH_DEVICE_SEARCH_PARAMS search{};
        search.dwSize = sizeof(search);
        search.fReturnAuthenticated = TRUE;
        search.fReturnRemembered = TRUE;
        search.fReturnConnected = TRUE;
        search.fIssueInquiry = FALSE;
        search.hRadio = radio;
        BLUETOOTH_DEVICE_INFO info{sizeof(info)};
        auto devices = BluetoothFindFirstDevice(&search, &info);
        if (devices) {
            do {
                if (_wcsicmp(info.szName, L"WF-C710N") != 0) continue;
                const auto* b = info.Address.rgBytes;
                char address[18]{};
                snprintf(address, sizeof(address), "%02X:%02X:%02X:%02X:%02X:%02X", b[5],b[4],b[3],b[2],b[1],b[0]);
                bool duplicate = false;
                for (const auto& d : matches) duplicate |= d.address == address;
                if (!duplicate) matches.push_back({address, info.fConnected != FALSE});
            } while (BluetoothFindNextDevice(devices, &info));
            BluetoothFindDeviceClose(devices);
        }
        CloseHandle(radio);
    } while (BluetoothFindNextRadio(radios, &radio));
    BluetoothFindRadioClose(radios);
    if (matches.empty()) throw std::runtime_error("WF-C710N is not paired. Pair it in Windows Bluetooth settings.");
    if (matches.size() != 1) throw std::runtime_error("More than one WF-C710N is paired. Explicit device selection is required.");
    if (!matches[0].connected) throw NotConnected("WF-C710N is paired but not connected. Connect it manually, then run the probe again.");
    return matches[0];
}

struct Session {
    HANDLE transport_owner{};
    MDRConnectionWindows* classic{};
    MDRConnectionWindowsBLE* ble{};
    MDRConnection* connection{};
    std::unique_ptr<EventClassicTransport> event_classic;
    MDRHeadphones* headphones{};
    bool battery_reported{}, noise_reported{}, eq_reported{};
    uint32_t tx{}, rx{};
    bool capture_eq_error{};
    std::array<unsigned char,128> last_rx{};
    int last_rx_size{};
    ~Session() {
        if (headphones) mdrHeadphonesDestroy(headphones);
        if (connection) mdrConnectionDisconnect(connection);
        if (classic) mdrConnectionWindowsDestroy(classic);
        if (ble) mdrConnectionWindowsBLEDestroy(ble);
        if(transport_owner){ReleaseMutex(transport_owner);CloseHandle(transport_owner);}
    }
    std::string text(MDRText field, uint32_t index = 0) const {
        char value[512]{};
        uint32_t size = sizeof(value);
        return mdrHeadphonesGetText(headphones, field, index, value, &size) == MDR_RESULT_OK ? value : "";
    }
    MDREvent poll() {
        MDREvent event{};
        auto result = mdrHeadphonesPoll(headphones, &event);
        if (result != MDR_RESULT_OK) {
            auto detail=text(MDR_TEXT_LAST_ERROR);
            if(capture_eq_error && detail.find("EqEbb")!=std::string::npos){
                std::ostringstream hex;hex<<"; rejected EQ frame=";
                for(int i=0;i<last_rx_size;++i)hex<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(last_rx[i]);
                detail+=hex.str();
            }
            throw std::runtime_error("Protocol: " + std::string(mdrResultString(result)) + "; " + detail);
        }
        battery_reported |= event == MDR_EVENT_BATTERY_CHANGED;
        noise_reported |= event == MDR_EVENT_NOISE_CONTROL_CHANGED;
        eq_reported |= event == MDR_EVENT_EQUALIZER_CHANGED;
        if (event == MDR_EVENT_NONE) std::this_thread::sleep_for(2ms);
        return event;
    }
    void until(MDREvent wanted, std::chrono::milliseconds timeout) {
        auto deadline = Clock::now() + timeout;
        while (Clock::now() < deadline) {
            auto event = poll();
            if (event == wanted) return;
        }
        throw std::runtime_error("Protocol operation timed out (sent=" + std::to_string(tx) + ", received=" + std::to_string(rx) + "); no automatic retry was attempted.");
    }
    HANDLE wait_handle() const {return event_classic?event_classic->event():nullptr;}
    void connect(const Device& device, bool use_ble, bool v1, bool event_driven=false) {
        HANDLE token{};
        if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))throw std::runtime_error("Could not identify the current Windows user.");
        DWORD bytes{};GetTokenInformation(token,TokenUser,nullptr,0,&bytes);std::vector<BYTE> user(bytes);
        BOOL identified=GetTokenInformation(token,TokenUser,user.data(),bytes,&bytes);CloseHandle(token);
        LPWSTR sid{};
        if(!identified || !ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid,&sid))throw std::runtime_error("Could not identify the current Windows user.");
        auto owner_name=std::wstring(L"Local\\EarbudControls.Transport.")+sid;LocalFree(sid);
        HANDLE owner=CreateMutexW(nullptr,FALSE,owner_name.c_str());
        if(!owner)throw std::runtime_error("Could not create the Bluetooth connection lock.");
        auto acquired=WaitForSingleObject(owner,0);
        if(acquired!=WAIT_OBJECT_0 && acquired!=WAIT_ABANDONED){CloseHandle(owner);throw std::runtime_error("Another Bandwave instance or hardware probe owns the connection. Close its controls and wait 15 seconds before retrying.");}
        transport_owner=owner;
        if(event_driven&&!use_ble){event_classic=std::make_unique<EventClassicTransport>();connection=&event_classic->api;}
        else if (use_ble) { ble = mdrConnectionWindowsBLECreate(); if (ble) connection = mdrConnectionWindowsBLEGet(ble); }
        else { classic = mdrConnectionWindowsCreate(); if (classic) connection = mdrConnectionWindowsGet(classic); }
        if (!connection) throw std::runtime_error("Could not create Bluetooth transport.");
        auto start = Clock::now();
        auto result = mdrConnectionConnect(connection, device.address.c_str(), use_ble ? MDR_SERVICE_UUID_BLE : v1 ? MDR_SERVICE_UUID_V1 : MDR_SERVICE_UUID_V2);
        if (result != MDR_RESULT_OK && result != MDR_RESULT_INPROGRESS)
            throw std::runtime_error("Connect: " + std::string(mdrConnectionGetLastError(connection)));
        while (result == MDR_RESULT_INPROGRESS && Clock::now() - start < 5s) {
            auto ready = mdrConnectionPoll(connection, 50);
            if (ready == MDR_RESULT_OK) result = ready;
            else if (ready != MDR_RESULT_INPROGRESS && ready != MDR_RESULT_ERROR_TIMEOUT)
                throw std::runtime_error("Transport: " + std::string(mdrConnectionGetLastError(connection)));
        }
        if (result != MDR_RESULT_OK) throw std::runtime_error("Bluetooth connection timed out after five seconds.");
        check(mdrHeadphonesCreate(MDR_ABI_VERSION, connection, v1 ? MDR_PROTOCOL_V1 : MDR_PROTOCOL_V2, &headphones), "Create engine");
        mdrHeadphonesSetPacketCallback(headphones, [](void* user, MDRPacketDirection direction, const unsigned char* data, int size) {
            auto& self = *static_cast<Session*>(user);
            if (direction == MDR_PACKET_DIRECTION_RX) {
                ++self.rx;
                if(self.capture_eq_error){self.last_rx_size=size<128?size:128;if(self.last_rx_size>0)std::memcpy(self.last_rx.data(),data,self.last_rx_size);}
            } else ++self.tx;
        }, this);
        check(mdrHeadphonesSetTimeout(headphones, MDR_TIMEOUT_DEFAULT, 3000), "Set command timeout");
        check(mdrHeadphonesRequestInit(headphones), "Initialize");
        until(MDR_EVENT_INITIALIZE_COMPLETE, 5s);
        if (text(MDR_TEXT_MODEL_NAME) != "WF-C710N") throw std::runtime_error("Unexpected or unverified model identity; no setting writes allowed.");
        check(mdrHeadphonesRequestSync(headphones), "Refresh batteries");
        until(MDR_EVENT_SYNC_COMPLETE, 3s);
    }
    MDRFeatureAvailability feature(MDRFeature id) const {
        MDRFeatureAvailability value = MDR_AVAILABILITY_UNKNOWN;
        if (mdrHeadphonesGetFeature(headphones, id, &value) != MDR_RESULT_OK) return MDR_AVAILABILITY_UNKNOWN;
        return value;
    }
    template<class Predicate> void commit_confirmed(Predicate confirmed) {
        check(mdrHeadphonesRequestCommit(headphones), "Commit setting");
        until(MDR_EVENT_APPLY_COMPLETE, 3s);
        auto deadline = Clock::now() + 3s;
        while (Clock::now() < deadline) {
            poll();
            if(confirmed())return;
        }
        throw std::runtime_error("The command was acknowledged but its resulting state was not confirmed.");
    }
    void write_noise(const MDRNoiseControl& wanted) {
        if (!noise_reported) throw std::runtime_error("Noise state has not been reported; refusing to overwrite unknown state.");
        check(mdrHeadphonesSetNoiseControl(headphones, &wanted), "Stage noise change");
        commit_confirmed([&]{MDRNoiseControl actual{};check(mdrHeadphonesGetNoiseControl(headphones,MDR_PROPERTY_CURRENT,&actual),"Read noise change");
            return actual.mode==wanted.mode && actual.ambient_level==wanted.ambient_level && actual.focus_on_voice==wanted.focus_on_voice;});
    }
};


