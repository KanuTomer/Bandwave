#pragma once
// Messages between the app (EarbudControls.UI.exe) and the owner (EarbudControls.exe) over a
// per-user, per-session, local-only named pipe. Each message is a 4-byte little-endian length and
// that many bytes of UTF-8 JSON. Anything oversized or malformed ends the connection.
//
// Owner -> app: {"type":"snapshot", ...device state..., "library":{...}}
//               {"type":"result","id":n,"accepted":bool}        a command was queued or refused
//               {"type":"show"}                                  bring the window forward
// App -> owner: {"type":"hello","version":1}
//               {"type":"command","id":n,"generation":g,"op":"...", ...}
//               {"type":"library","library":{...}}              the owner validates and saves it
//               {"type":"interest","active":bool}               the window is open and visible
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace earbud::ipc {
inline constexpr int kVersion = 1;
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
