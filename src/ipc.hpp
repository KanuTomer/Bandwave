#pragma once
// Messages between the app (EarbudControls.UI.exe) and the owner (EarbudControls.exe) over a
// per-user, per-session, local-only named pipe. Each message is a 4-byte little-endian length and
// that many bytes of UTF-8 JSON. Anything oversized or malformed ends the connection.
//
// Version 2 (0.10.0): several devices. Every device message names its device by Bluetooth address,
// which never leaves this PC. The app and the owner ship together, so the owner ignores a client
// that doesn't greet with this version.
//
// Owner -> app: {"type":"devices","focused":"AA:..","shown":"AA:..","list":[{"address","name",
//                 "model","displayName","form","supported","connected","control","ready",
//                 "batteries":[l,r,c]}]}                 every paired Sony device, as Windows has it;
//                                                        focused: open or last open in the app;
//                                                        shown: what the widget shows
//               {"type":"snapshot","address":"AA:..","model":"wf-c710n","control":"idle|opening|
//                 ready|in_use|unavailable","connected":bool, ...device state..., "library":{...}}
//               {"type":"result","id":n,"address":"AA:..","accepted":bool}
//               {"type":"show","screen":"devices"}       bring the window forward
// App -> owner: {"type":"hello","version":2}
//               {"type":"view","visible":bool,"screen":"devices|device","address":"AA:..|"}
//               {"type":"focus","address":"AA:.."}      the app opened that device's screen
//               {"type":"setting","key":"release_when_hidden","value":0|1}   the app's, for every device
//               {"type":"command","id":n,"address":"AA:..","generation":g,"op":"...", ...}
//               {"type":"library","address":"AA:..","library":{...}}   validated, then saved
// A command or library for an address that isn't a paired, supported device is refused, and so is
// a command built against an earlier connection of that device.#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace earbud::ipc {
inline constexpr int kVersion = 2;
inline constexpr uint32_t kMaxMessage = 256 * 1024;

inline std::string frame(std::string_view json) {
    const auto size = static_cast<uint32_t>(json.size());
    std::string out(4, '\0');
    for (int i = 0; i < 4; ++i) out[static_cast<size_t>(i)] = static_cast<char>((size >> (8 * i)) & 0xFF);
    out.append(json);
    return out;
}

// Collects bytes as they arrive and hands back whole messages. Once it fails it stays failed.
class Reader {
public:
    void feed(std::string_view bytes) { if (!failed_) buffer_.append(bytes); if (buffer_.size() > kMaxMessage + 4) failed_ = true; }
    std::optional<std::string> next() {
        if (failed_ || buffer_.size() < 4) return std::nullopt;
        uint32_t size = 0;
        for (int i = 0; i < 4; ++i) size |= static_cast<uint32_t>(static_cast<unsigned char>(buffer_[static_cast<size_t>(i)])) << (8 * i);
        if (size == 0 || size > kMaxMessage) { failed_ = true; return std::nullopt; }
        if (buffer_.size() < 4 + size) return std::nullopt;
        std::string message = buffer_.substr(4, size);
        buffer_.erase(0, 4 + size);
        return message;
    }
    bool failed() const { return failed_; }
private:
    std::string buffer_;
    bool failed_ = false;
};

// Pipe names are scoped to the user's SID and logon session; the owner also sets a DACL for the
// user alone and rejects remote clients.
inline std::wstring app_pipe(std::wstring const& activation_pipe) { return activation_pipe + L".app"; }
}
