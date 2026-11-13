// Per-device storage: the device index, the move from the single-device layout, and recovery.
#include <windows.h>
#include <winrt/base.h>
#include "device_store.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>

#define CHECK(condition) do { if (!(condition)) { std::printf("FAILED line %d: %s\n", __LINE__, #condition); std::exit(1); } } while (0)

namespace fs = std::filesystem;
static void write(fs::path const& path, const char* text) { fs::create_directories(path.parent_path()); std::ofstream(path, std::ios::binary) << text; }
static std::string read(fs::path const& path) { std::ifstream in(path, std::ios::binary); return {std::istreambuf_iterator<char>(in), {}}; }

int main() {
    winrt::init_apartment();
    using namespace bandwave;
    const auto base = fs::temp_directory_path() / (L"bandwave-device-store-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(base, ec);

    CHECK(valid_address("14:06:A7:28:D4:6E") && !valid_address("14:06:a7:28:d4:6e") && !valid_address("1406A728D46E") && !valid_address("") && !valid_address("..\\..\\x:00:00:00:00"));
    CHECK(device_folder(L"C:\\data", "14:06:A7:28:D4:6E") == fs::path(L"C:\\data") / L"devices" / L"1406A728D46E");

    {   // The index survives a round trip, and remembers what a device is.
        DeviceIndex index;
        CHECK(index.remember("AA:BB:CC:DD:EE:01", "wf-c710n", L"WF-C710N"));
        CHECK(!index.remember("AA:BB:CC:DD:EE:01", "wf-c710n", L"WF-C710N"));
        CHECK(index.remember("AA:BB:CC:DD:EE:01", "", L"Kanu's buds") && index.find("AA:BB:CC:DD:EE:01")->model == "wf-c710n"); // a rename keeps the model
        CHECK(index.remember("AA:BB:CC:DD:EE:02", "", L"WH-1000XM5"));
        index.focused = "AA:BB:CC:DD:EE:01"; index.logging = true; index.release = 1;
        auto again = index_from_text(to_text(index));
        CHECK(again && *again == index);
        CHECK(!index_from_text(L"") && !index_from_text(L"not json"));
        auto odd = index_from_text(LR"({"focused":"../x","known":[{"address":"bad"},{"address":"AA:BB:CC:DD:EE:03","model":"wf-c710n","name":"A"},{"address":"AA:BB:CC:DD:EE:03"}]})");
        CHECK(odd && odd->focused.empty() && odd->known.size() == 1); // a bad address is dropped, a repeat is ignored
    }
    {   // 0.9.0 with a remembered device: its files are copied to its own folder; the originals stay.
        const auto root = base / L"with-address";
        write(root / L"state.json", R"({"address":"14:06:A7:28:D4:6E","firmware":"1.0.5","logging":true,"checked":5})");
        write(root / L"library.json", R"({"version":1})");
        migrate_legacy(root);
        const auto folder = device_folder(root, "14:06:A7:28:D4:6E");
        CHECK(read(folder / L"state.json") == read(root / L"state.json") && read(folder / L"library.json") == R"({"version":1})");
        CHECK(fs::exists(root / L"state.json") && fs::exists(root / L"library.json"));
        auto index = load_index(root);
        CHECK(index.focused == "14:06:A7:28:D4:6E" && index.logging && index.release == 0 && index.known.size() == 1 && index.known[0].model == "wf-c710n");
        CHECK(index_from_text(LR"({"focused":""})")->release == -1); // not set before 0.10.1: taken from the devices
        // A second start changes nothing, even if the old files have since changed.
        write(folder / L"library.json", R"({"version":1,"edited":true})");
        migrate_legacy(root);
        CHECK(read(folder / L"library.json") == R"({"version":1,"edited":true})");
    }
    {   // 0.9.0 that never met its earbuds: the library waits, then goes to the first WF-C710N.
        const auto root = base / L"no-address";
        write(root / L"library.json", R"({"version":1,"mine":true})");
        migrate_legacy(root);
        CHECK(load_index(root).focused.empty() && fs::exists(root / L"devices" / L"_unassigned" / L"library.json") && fs::exists(root / L"library.json"));
        adopt_unassigned(root, "AA:BB:CC:DD:EE:02", "wh-1000xm5"); // another model's device doesn't take it
        CHECK(fs::exists(root / L"devices" / L"_unassigned" / L"library.json"));
        adopt_unassigned(root, "AA:BB:CC:DD:EE:01", "wf-c710n");
        CHECK(read(device_folder(root, "AA:BB:CC:DD:EE:01") / L"library.json") == R"({"version":1,"mine":true})" && !fs::exists(root / L"devices" / L"_unassigned" / L"library.json"));
        adopt_unassigned(root, "AA:BB:CC:DD:EE:03", "wf-c710n"); // only once
        CHECK(!fs::exists(device_folder(root, "AA:BB:CC:DD:EE:03") / L"library.json"));
    }
    {   // A fresh install has nothing to move and gets an empty index.
        const auto root = base / L"fresh";
        fs::create_directories(root);
        migrate_legacy(root);
        CHECK(fs::exists(index_path(root)) && load_index(root) == DeviceIndex{} && !fs::exists(root / L"devices"));
    }
    {   // An unreadable index is set aside, never overwritten in place.
        const auto root = base / L"corrupt";
        write(index_path(root), "{{{");
        CHECK(load_index(root) == DeviceIndex{} && !fs::exists(index_path(root)));
        bool kept = false;
        for (auto const& entry : fs::directory_iterator(root)) kept |= entry.path().filename().wstring().starts_with(L"devices.unreadable-");
        CHECK(kept);
    }
    fs::remove_all(base, ec);
    std::printf("device store tests passed\n");
    return 0;
}
