// Model profiles and Sony device classification. No Bluetooth calls.
#include "sony_devices.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(condition) do { if (!(condition)) { std::printf("FAILED line %d: %s\n", __LINE__, #condition); std::exit(1); } } while (0)

int main(int argc, char** argv) {
    using namespace bandwave;
    // --list: what this PC has paired (a hardware check, run by hand).
    if (argc > 1 && !std::strcmp(argv[1], "--list")) {
        for (auto const& d : list_paired_sony())
            std::printf("%s  %ls  %s  %s\n", d.address.c_str(), d.windows_name.c_str(), d.connected ? "connected" : "not connected", d.profile ? std::string(d.profile->id).c_str() : "no profile");
        return 0;
    }
    // Every profile is complete, and no two claim the same id or Windows name.
    auto profiles = all_profiles();
    CHECK(!profiles.empty());
    for (size_t i = 0; i < profiles.size(); ++i) {
        auto const& p = profiles[i];
        CHECK(!p.id.empty() && !p.display_name.empty() && !p.noun.empty() && !p.reported_model.empty());
        CHECK(!p.windows_names.empty() && !p.art_folder.empty() && !p.widget_art.empty());
        CHECK(p.band_min < p.band_max);
        for (auto label : p.band_labels) CHECK(!label.empty());
        for (auto c : p.id) CHECK(!(c >= 'A' && c <= 'Z'));
        for (size_t j = 0; j < i; ++j) {
            CHECK(profiles[j].id != p.id);
            for (auto a : profiles[j].windows_names) for (auto b : p.windows_names) CHECK(!same_name(a, b));
        }
    }
    // The first model.
    auto* wf = profile_by_id("wf-c710n");
    CHECK(wf && wf->display_name == L"WF-C710N" && wf->form == FormFactor::Buds && wf->find_eq);
    CHECK(profile_for_windows_name(L"WF-C710N") == wf);
    CHECK(profile_for_windows_name(L"wf-c710n") == wf);
    CHECK(profile_for_reported_model("WF-C710N") == wf);
    CHECK(!profile_by_id("WF-C710N") && !profile_for_windows_name(L"WF-C710") && !profile_for_windows_name(L"WH-1000XM5"));
    CHECK(wf->is_verified("noise") && wf->is_verified("general_3") && wf->is_verified("touch_left"));
    CHECK(!wf->is_verified("spatial") && !wf->is_verified("general"));

    // Classification.
    const GUID v2[]{kSonyServices[0]}, v1[]{kSonyServices[1]};
    const GUID other[]{{0x0000110B, 0x0000, 0x1000, {0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB}}}; // an audio sink
    auto supported = classify("AA:BB:CC:DD:EE:01", L"WF-C710N", true, other);
    CHECK(supported.sony && supported.profile == wf && supported.connected);
    auto le = classify("AA:BB:CC:DD:EE:02", L"LE_WF-C710N", false, {});
    CHECK(le.sony && le.profile == wf && !le.connected);
    auto unsupported = classify("AA:BB:CC:DD:EE:03", L"WH-1000XM5", true, {});
    CHECK(unsupported.sony && !unsupported.profile);
    auto renamed = classify("AA:BB:CC:DD:EE:04", L"Kanu's buds", true, v2);
    CHECK(renamed.sony && !renamed.profile); // its profile comes from what it reports once read
    CHECK(classify("AA:BB:CC:DD:EE:05", L"Old Sony", false, v1).sony);
    CHECK(classify("AA:BB:CC:DD:EE:06", L"LinkBuds S", false, {}).sony);
    CHECK(classify("AA:BB:CC:DD:EE:07", L"ULT WEAR", false, {}).sony);
    CHECK(!classify("AA:BB:CC:DD:EE:08", L"Tribit XSound Plus 2", true, other).sony);
    CHECK(!classify("AA:BB:CC:DD:EE:09", L"WFH speaker", true, {}).sony);
    CHECK(!classify("AA:BB:CC:DD:EE:0A", L"", true, {}).sony);

    // One device seen twice is listed once, connected if either sighting was.
    std::vector<PairedDevice> list;
    merge(list, classify("AA:BB:CC:DD:EE:01", L"Kanu's buds", false, v2));
    merge(list, classify("AA:BB:CC:DD:EE:01", L"WF-C710N", true, {}));
    merge(list, unsupported);
    CHECK(list.size() == 2 && list[0].connected && list[0].profile == wf && list[0].windows_name == L"Kanu's buds");

    BLUETOOTH_ADDRESS address{};
    address.ullLong = 0x0000A1B2C3D4E5F6ULL;
    CHECK(format_address(address) == "A1:B2:C3:D4:E5:F6");
    std::printf("model profile tests passed\n");
    return 0;
}
