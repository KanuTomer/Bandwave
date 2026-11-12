#pragma once
// The app's side of the pipe to EarbudControls.exe (see src/ipc.hpp). A background thread connects,
// retries while the owner starts, and hands every complete message to the UI thread. Sends are
// queued and written by the same thread, so the UI never blocks on the pipe.
#include "../src/ipc.hpp"
#include <windows.h>
#include <sddl.h>
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace earbud {
inline std::wstring owner_pipe_name() {
    HANDLE token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    DWORD bytes{};
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    std::string buffer(bytes, '\0');
    std::wstring sid;
    if (GetTokenInformation(token, TokenUser, buffer.data(), bytes, &bytes)) {
        LPWSTR raw{};
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &raw)) { sid = raw; LocalFree(raw); }
    }
    CloseHandle(token);
    DWORD session{};
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    return sid.empty() ? L"" : ipc::app_pipe(L"\\\\.\\pipe\\EarbudControls." + sid + L"." + std::to_wstring(session));
}

class OwnerLink {
public:
    // on_message and on_state run on whatever thread `post` delivers to (the UI thread).
    OwnerLink(std::function<void(std::function<void()>)> post, std::function<void(std::string)> on_message, std::function<void(bool)> on_state)
        : post_(std::move(post)), on_message_(std::move(on_message)), on_state_(std::move(on_state)) {
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        wake_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        thread_ = std::thread([this] { run(); });
    }
    ~OwnerLink() { SetEvent(stop_); if (thread_.joinable()) thread_.join(); CloseHandle(stop_); CloseHandle(wake_); }
    void send(std::string json) {
        if (!connected_) return;
        { std::lock_guard lock(mutex_); if (outbox_.size() > 64) outbox_.pop_front(); outbox_.push_back(ipc::frame(json)); }
        SetEvent(wake_);
    }
    bool connected() const { return connected_; }

private:
    std::function<void(std::function<void()>)> post_;
    std::function<void(std::string)> on_message_;
    std::function<void(bool)> on_state_;
    HANDLE stop_{}, wake_{};
    std::thread thread_;
    std::mutex mutex_;
    std::deque<std::string> outbox_;
    std::atomic<bool> connected_{};

    void deliver(ipc::Reader& reader) { while (auto m = reader.next()) post_([this, m = *m] { on_message_(m); }); }
    void run() {
        const auto name = owner_pipe_name();
        while (WaitForSingleObject(stop_, 0) != WAIT_OBJECT_0) {
            HANDLE pipe = name.empty() ? INVALID_HANDLE_VALUE : CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
            if (pipe == INVALID_HANDLE_VALUE) { if (WaitForSingleObject(stop_, 500) == WAIT_OBJECT_0) return; continue; }
            { std::lock_guard lock(mutex_); outbox_.clear(); }
            connected_ = true;
            post_([this] { on_state_(true); });
            send(R"({"type":"hello","version":2})");
            HANDLE read_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            OVERLAPPED ov{};
            ipc::Reader reader;
            char buffer[8192];
            bool reading = false, alive = true;
            while (alive) {
                if (!reading) {
                    ResetEvent(read_event); ov = {}; ov.hEvent = read_event; DWORD got{};
                    if (ReadFile(pipe, buffer, sizeof(buffer), &got, &ov)) { reader.feed({buffer, got}); deliver(reader); if (reader.failed()) break; continue; }
                    if (GetLastError() != ERROR_IO_PENDING) break;
                    reading = true;
                }
                HANDLE waits[] = {stop_, read_event, wake_};
                const auto waited = WaitForMultipleObjects(3, waits, FALSE, INFINITE);
                if (waited == WAIT_OBJECT_0 + 1) {
                    DWORD got{}; reading = false;
                    if (!GetOverlappedResult(pipe, &ov, &got, FALSE)) break;
                    reader.feed({buffer, got}); deliver(reader); if (reader.failed()) break;
                } else if (waited == WAIT_OBJECT_0 + 2) {
                    std::deque<std::string> pending;
                    { std::lock_guard lock(mutex_); pending.swap(outbox_); ResetEvent(wake_); }
                    for (auto& bytes : pending) {
                        OVERLAPPED wov{}; wov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr); DWORD put{};
                        BOOL ok = WriteFile(pipe, bytes.data(), static_cast<DWORD>(bytes.size()), &put, &wov);
                        if (!ok && GetLastError() == ERROR_IO_PENDING) ok = WaitForSingleObject(wov.hEvent, 5000) == WAIT_OBJECT_0 && GetOverlappedResult(pipe, &wov, &put, FALSE);
                        CloseHandle(wov.hEvent);
                        if (!ok) { alive = false; break; }
                    }
                } else alive = false;
            }
            CancelIoEx(pipe, nullptr);
            CloseHandle(pipe); CloseHandle(read_event);
            connected_ = false;
            post_([this] { on_state_(false); });
            if (WaitForSingleObject(stop_, 1000) == WAIT_OBJECT_0) return;
        }
    }
};
}
