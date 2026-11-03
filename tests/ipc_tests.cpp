#include "ipc.hpp"
#include <cstdio>
#include <cstdlib>
#define CHECK(condition) do { if (!(condition)) { std::printf("FAILED line %d: %s\n", __LINE__, #condition); std::exit(1); } } while (0)
using namespace earbud::ipc;

int main() {
    // Whole messages come back exactly, in order, however the bytes are split.
    const std::string a = R"({"type":"hello","version":1})", b = R"({"type":"interest","active":true})";
    const std::string stream = frame(a) + frame(b);
    for (size_t chunk : {1u, 3u, 7u, 1000u}) {
        Reader reader;
        std::string got;
        for (size_t i = 0; i < stream.size(); i += chunk) {
            reader.feed(std::string_view(stream).substr(i, chunk));
            while (auto message = reader.next()) got += *message + "|";
        }
        CHECK(got == a + "|" + b + "|" && !reader.failed());
    }
    // A partial message waits; nothing is invented.
    Reader partial;
    partial.feed(frame(a).substr(0, 10));
    CHECK(!partial.next() && !partial.failed());

    // Oversized or empty lengths end the connection, and it stays ended.
    Reader huge;
    std::string length(4, '\0');
    const uint32_t big = kMaxMessage + 1;
    for (int i = 0; i < 4; ++i) length[static_cast<size_t>(i)] = static_cast<char>((big >> (8 * i)) & 0xFF);
    huge.feed(length);
    CHECK(!huge.next() && huge.failed());
    huge.feed(frame(a));
    CHECK(!huge.next() && huge.failed());
    Reader empty;
    empty.feed(std::string(4, '\0'));
    CHECK(!empty.next() && empty.failed());
    // A flood without a complete frame is cut off too.
    Reader flood;
    std::string start(4, '\0');
    const uint32_t most = kMaxMessage;
    for (int i = 0; i < 4; ++i) start[static_cast<size_t>(i)] = static_cast<char>((most >> (8 * i)) & 0xFF);
    flood.feed(start);
    flood.feed(std::string(kMaxMessage + 10, 'x'));
    CHECK(flood.failed());
    std::printf("ipc tests passed\n");
    return 0;
}
