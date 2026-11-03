#pragma once
// Levels of what this PC is playing, split into the seven bands that light the background. It uses
// WASAPI loopback on the default output. It never sees phone audio, and it hears the music before
// the earbuds apply their EQ. It only listens when the default output is the WF-C710N, so other
// speakers never light the app; review builds may accept any output. Adapted from Tribit Connect.
#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <audioclient.h>
#include <ks.h>
#include <ksmedia.h>
#include <winrt/base.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "audio_analysis.hpp"

namespace dusk {
class Spectrum {
public:
    ~Spectrum() { stop(); }
    void start() { if (running_.exchange(true)) return; worker_ = std::thread([this] { run(); }); }
    void stop() {
        running_ = false;
        if (worker_.joinable()) worker_.join();
        playing_ = false;
        for (auto& level : levels_) level = 0;
    }
    bool playing() const { return playing_; }
    void accept_any_output(bool any) { any_output_ = any; }
    std::array<float, kBands> levels() const {
        std::array<float, kBands> out{};
        for (size_t i = 0; i < kBands; ++i) out[i] = levels_[i];
        return out;
    }

private:
    static constexpr int kSize = 2048, kHop = 1024;
    std::thread worker_;
    std::atomic<bool> running_{}, playing_{}, any_output_{};
    std::array<std::atomic<float>, kBands> levels_{};

    static bool is_earbuds(IMMDevice* device) {
        winrt::com_ptr<IPropertyStore> store;
        if (FAILED(device->OpenPropertyStore(STGM_READ, store.put()))) return false;
        PROPVARIANT name; PropVariantInit(&name);
        bool match = false;
        if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR && name.pwszVal) {
            std::wstring text = name.pwszVal;
            std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return static_cast<wchar_t>(towupper(c)); });
            match = text.find(L"WF-C710N") != std::wstring::npos;
        }
        PropVariantClear(&name);
        return match;
    }
    void idle(int ms) { for (int t = 0; t < ms && running_; t += 20) Sleep(20); }
    void run() {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        while (running_) {
            try { capture(); } catch (...) {}
            playing_ = false;
            idle(2000); // no usable output, or the device changed: look again shortly
        }
        winrt::uninit_apartment();
    }
    void capture() {
        auto enumerator = winrt::create_instance<IMMDeviceEnumerator>(__uuidof(MMDeviceEnumerator));
        winrt::com_ptr<IMMDevice> device;
        winrt::check_hresult(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.put()));
        LPWSTR id{};
        device->GetId(&id);
        std::wstring device_id = id ? id : L"";
        CoTaskMemFree(id);
        if (!any_output_ && !is_earbuds(device.get())) return; // looked at again shortly, in run()
        winrt::com_ptr<IAudioClient> client;
        winrt::check_hresult(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.put_void()));
        WAVEFORMATEX* format{};
        winrt::check_hresult(client->GetMixFormat(&format));
        std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> owned(format, &CoTaskMemFree);
        const bool is_float = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT || (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
            reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        if (!(is_float && format->wBitsPerSample == 32) && format->wBitsPerSample != 16) return;
        winrt::check_hresult(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, format, nullptr));
        winrt::com_ptr<IAudioCaptureClient> capture;
        winrt::check_hresult(client->GetService(__uuidof(IAudioCaptureClient), capture.put_void()));
        winrt::check_hresult(client->Start());
        const int channels = format->nChannels;
        const float rate = static_cast<float>(format->nSamplesPerSec);
        AudioAnalysis analysis;
        auto last_sound = GetTickCount64(), last_check = last_sound;
        while (running_) {
            Sleep(15);
            UINT32 packet = 0;
            if (FAILED(capture->GetNextPacketSize(&packet))) break;
            while (packet) {
                BYTE* data{};
                UINT32 frames{};
                DWORD flags{};
                if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) return;
                for (UINT32 f = 0; f < frames; ++f) {
                    float sum = 0;
                    if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) for (int c = 0; c < channels; ++c)
                        sum += is_float ? reinterpret_cast<float*>(data)[f * channels + c] : reinterpret_cast<int16_t*>(data)[f * channels + c] / 32768.f;
                    analysis.push(sum / channels, rate, [&](auto const& bands) {
                        for (size_t i = 0; i < kBands; ++i) levels_[i] = bands[i];
                        if (*std::max_element(bands.begin(), bands.end()) > .02f) last_sound = GetTickCount64();
                    });
                }
                capture->ReleaseBuffer(frames);
                if (FAILED(capture->GetNextPacketSize(&packet))) return;
            }
            const auto now = GetTickCount64();
            // Silence renders nothing at all, so a quiet stretch means playback stopped.
            playing_ = now - last_sound < 1500;
            if (!playing_) for (auto& level : levels_) level = 0;
            // Follow a change of the default output.
            if (now - last_check > 2000) {
                last_check = now;
                winrt::com_ptr<IMMDevice> current;
                if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, current.put()))) break;
                LPWSTR now_id{};
                current->GetId(&now_id);
                const bool same = now_id && device_id == now_id;
                CoTaskMemFree(now_id);
                if (!same) break;
            }
        }
        client->Stop();
    }

};
}
