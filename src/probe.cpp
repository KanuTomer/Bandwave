#include "device.hpp"

static const char* availability(MDRFeatureAvailability value) {
    return value == MDR_AVAILABILITY_AVAILABLE ? "advertised" : value == MDR_AVAILABILITY_UNAVAILABLE ? "unsupported" : "unverified";
}

static std::vector<int32_t> eq_bands(Session& session){
    int32_t bands[10]{};uint32_t count=10;
    check(mdrHeadphonesGetEqualizerBands(session.headphones,MDR_PROPERTY_CURRENT,bands,&count),"Read EQ bands");
    return {bands,bands+count};
}
static void verify_eq(Session& session){
    session.capture_eq_error=true;
    if(!session.eq_reported || session.feature(MDR_FEATURE_EQUALIZER)!=MDR_AVAILABILITY_AVAILABLE)throw std::runtime_error("EQ was not reported; no EQ write was attempted.");
    MDREqualizer original{};check(mdrHeadphonesGetEqualizer(session.headphones,MDR_PROPERTY_CURRENT,&original),"Read original EQ");
    if(original.preset!=MDR_EQ_CUSTOM && original.preset!=MDR_EQ_USER_1 && original.preset!=MDR_EQ_USER_2)throw std::runtime_error("Select an editable custom EQ preset before testing; other preset slots are left untouched.");
    auto original_bands=eq_bands(session);
    if(original_bands.size()!=5)throw std::runtime_error("This hardware verification expects the WF-C710N five-band EQ.");
    auto matches=[&](uint32_t preset,int bass,const std::vector<int32_t>& bands){MDREqualizer current{};check(mdrHeadphonesGetEqualizer(session.headphones,MDR_PROPERTY_CURRENT,&current),"Read EQ result");return current.preset==preset&&current.clear_bass==bass&&eq_bands(session)==bands;};
    auto restore=[&]{
        check(mdrHeadphonesSetEqualizerPreset(session.headphones,original.preset),"Restore EQ preset");
        check(mdrHeadphonesSetEqualizerBands(session.headphones,original_bands.data(),static_cast<uint32_t>(original_bands.size())),"Restore EQ bands");
        check(mdrHeadphonesSetEqualizerClearBass(session.headphones,original.clear_bass),"Restore Clear Bass");
        session.commit_confirmed([&]{return matches(original.preset,original.clear_bass,original_bands);});
    };
    const char* stage="preset selection";
    try{
        check(mdrHeadphonesSetEqualizerPreset(session.headphones,MDR_EQ_OFF),"Test EQ preset");
        session.commit_confirmed([&]{MDREqualizer current{};check(mdrHeadphonesGetEqualizer(session.headphones,MDR_PROPERTY_CURRENT,&current),"Read preset result");return current.preset==MDR_EQ_OFF;});
        stage="restoration after preset selection";restore();
        stage="custom EQ band write";
        auto bands=original_bands;bands[0]+=bands[0]<10?1:-1;
        check(mdrHeadphonesSetEqualizerBands(session.headphones,bands.data(),static_cast<uint32_t>(bands.size())),"Test custom EQ");
        session.commit_confirmed([&]{return matches(original.preset,original.clear_bass,bands);});
        stage="Clear Bass write";
        int bass=original.clear_bass+(original.clear_bass<10?1:-1);
        check(mdrHeadphonesSetEqualizerClearBass(session.headphones,bass),"Test Clear Bass");
        session.commit_confirmed([&]{return matches(original.preset,bass,bands);});
    }catch(...){std::cerr<<"EQ verification failed during "<<stage<<".\n";try{restore();}catch(...){std::cerr<<"EQ restoration was not confirmed. Check the preset, bands, and Clear Bass in Sony Sound Connect.\n";}throw;}
    try{restore();}catch(...){std::cerr<<"EQ test ran but final restoration was not confirmed. Check Sony Sound Connect.\n";throw;}
}

// Reads every advertised preset's five bands and Clear Bass by selecting each in turn, then checks
// whether Clear Bass belongs to each slot or is one shared setting. Everything is restored at the end.
static void survey_eq(Session& session){
    session.capture_eq_error=true;
    if(!session.eq_reported || session.feature(MDR_FEATURE_EQUALIZER)!=MDR_AVAILABILITY_AVAILABLE)throw std::runtime_error("EQ was not reported; nothing was changed.");
    MDREqualizer original{};check(mdrHeadphonesGetEqualizer(session.headphones,MDR_PROPERTY_CURRENT,&original),"Read original EQ");
    const auto original_bands=eq_bands(session);
    const bool editable=[&](uint32_t p){return p==MDR_EQ_CUSTOM||p==MDR_EQ_USER_1||p==MDR_EQ_USER_2;}(original.preset);
    MDREqualizerPreset presets[64]{};uint32_t count=64;
    check(mdrHeadphonesGetEqualizerPresets(session.headphones,presets,&count),"Read preset list");
    const auto select=[&](uint32_t preset){
        check(mdrHeadphonesSetEqualizerPreset(session.headphones,preset),"Select preset");
        session.commit_confirmed([&]{MDREqualizer current{};check(mdrHeadphonesGetEqualizer(session.headphones,MDR_PROPERTY_CURRENT,&current),"Read preset");return current.preset==preset;});
    };
    const auto current_bass=[&]{MDREqualizer current{};check(mdrHeadphonesGetEqualizer(session.headphones,MDR_PROPERTY_CURRENT,&current),"Read Clear Bass");return current.clear_bass;};
    std::optional<int> manual_bass;
    const auto restore=[&]{
        if(manual_bass){select(MDR_EQ_CUSTOM);check(mdrHeadphonesSetEqualizerClearBass(session.headphones,*manual_bass),"Restore Manual Clear Bass");session.commit_confirmed([&]{return current_bass()==*manual_bass;});}
        select(original.preset);
        if(editable){check(mdrHeadphonesSetEqualizerBands(session.headphones,original_bands.data(),static_cast<uint32_t>(original_bands.size())),"Restore bands");}
        check(mdrHeadphonesSetEqualizerClearBass(session.headphones,original.clear_bass),"Restore Clear Bass");
        session.commit_confirmed([&]{return current_bass()==original.clear_bass&&(!editable||eq_bands(session)==original_bands);});
    };
    std::cout<<"{\n  \"result\": \"survey\",\n  \"original_preset\": "<<original.preset<<",\n  \"presets\": [";
    try{
        bool have_manual=false,have_user1=false,first=true;
        for(uint32_t i=0;i<count;++i){
            if(presets[i]==MDR_EQ_UNKNOWN)continue;
            select(presets[i]);
            auto bands=eq_bands(session);const int bass=current_bass();
            if(presets[i]==MDR_EQ_CUSTOM){have_manual=true;manual_bass=bass;}
            if(presets[i]==MDR_EQ_USER_1)have_user1=true;
            std::cout<<(first?"":",")<<"\n    {\"id\":"<<presets[i]<<",\"name\":"<<quoted(session.text(MDR_TEXT_EQUALIZER_PRESET_NAME,i))<<",\"clear_bass\":"<<bass<<",\"bands\":[";
            for(size_t b=0;b<bands.size();++b)std::cout<<(b?",":"")<<bands[b];
            std::cout<<"]}";first=false;
        }
        std::cout<<"\n  ],\n  \"clear_bass_scope\": ";
        if(have_manual&&have_user1){
            select(MDR_EQ_USER_1);const int user1=current_bass();
            select(MDR_EQ_CUSTOM);const int changed=*manual_bass+(*manual_bass<10?1:-1);
            check(mdrHeadphonesSetEqualizerClearBass(session.headphones,changed),"Test Manual Clear Bass");
            session.commit_confirmed([&]{return current_bass()==changed;});
            select(MDR_EQ_USER_1);const int after=current_bass();
            std::cout<<quoted(after==changed&&user1!=changed?"shared":after==user1?"per_slot":"unclear");
        }else std::cout<<"\"not_tested\"";
    }catch(...){std::cout<<"\n  ]\n}\n";std::cerr<<"EQ survey stopped early; restoring the original EQ.\n";try{restore();}catch(...){std::cerr<<"Restoration was not confirmed. Check the EQ in Sony Sound Connect.\n";}throw;}
    try{restore();}catch(...){std::cout<<"\n}\n";std::cerr<<"Survey finished but restoration was not confirmed. Check Sony Sound Connect.\n";throw;}
    std::cout<<",\n  \"restored\": true\n}\n";
}

static void snapshot(Session& s, bool verified_noise, bool restored, bool verified_eq) {
    std::cout << "{\n  \"result\": \"connected\",\n  \"engine_revision\": " << quoted(MDR_ENGINE_REVISION)
              << ",\n  \"engine_patches\": [\"wf-c710n-eq-notification-v1\"]"
              << ",\n  \"model\": " << quoted(s.text(MDR_TEXT_MODEL_NAME)) << ",\n  \"firmware\": " << quoted(s.text(MDR_TEXT_FIRMWARE_VERSION))
              << ",\n  \"features\": {";
    const std::pair<const char*, MDRFeature> features[] = {{"left_right_battery",MDR_FEATURE_BATTERY_LEFT_RIGHT},
        {"case_battery",MDR_FEATURE_BATTERY_CASE},{"noise_cancelling",MDR_FEATURE_NOISE_CANCELLING},
        {"ambient_sound",MDR_FEATURE_AMBIENT_SOUND},{"equalizer",MDR_FEATURE_EQUALIZER}};
    for (size_t i=0; i<std::size(features); ++i)
        std::cout << (i ? "," : "") << "\n    " << quoted(features[i].first) << ": " << quoted(availability(s.feature(features[i].second)));
    std::cout << "\n  },\n  \"battery_reported\": " << (s.battery_reported ? "true" : "false") << ",\n  \"batteries\": [";
    MDRBattery batteries[4]{};
    uint32_t count = 4;
    if (s.battery_reported && mdrHeadphonesGetBatteries(s.headphones,batteries,&count) == MDR_RESULT_OK) {
        for (uint32_t i=0; i<count; ++i) {
            if (i) std::cout << ",";
            std::cout << "{\"part\":" << batteries[i].part << ",\"present\":" << (batteries[i].present ? "true":"false") << ",\"percent\":";
            if (batteries[i].present) std::cout << batteries[i].level_percent; else std::cout << "null";
            std::cout << "}";
        }
    }
    std::cout << "],\n  \"noise_mode\": ";
    MDRNoiseControl noise{};
    if (s.noise_reported && mdrHeadphonesGetNoiseControl(s.headphones,MDR_PROPERTY_CURRENT,&noise) == MDR_RESULT_OK) std::cout << noise.mode;
    else std::cout << "null";
    std::cout << ",\n  \"eq_reported\": " << (s.eq_reported ? "true":"false");
    MDREqualizer eq{};
    if (s.eq_reported && mdrHeadphonesGetEqualizer(s.headphones,MDR_PROPERTY_CURRENT,&eq) == MDR_RESULT_OK) {
        std::cout << ",\n  \"eq_preset\": " << eq.preset << ",\n  \"clear_bass\": " << eq.clear_bass << ",\n  \"eq_bands\": [";
        int32_t bands[16]{}; uint32_t n=16;
        if (mdrHeadphonesGetEqualizerBands(s.headphones,MDR_PROPERTY_CURRENT,bands,&n) == MDR_RESULT_OK)
            for (uint32_t i=0; i<n; ++i) std::cout << (i ? ",":"") << bands[i];
        std::cout << "]";
        MDREqualizerPreset presets[64]{}; uint32_t preset_count=64;
        std::cout << ",\n  \"eq_presets\": [";
        if (mdrHeadphonesGetEqualizerPresets(s.headphones,presets,&preset_count) == MDR_RESULT_OK)
            for (uint32_t i=0; i<preset_count; ++i)
                std::cout << (i ? ",":"") << "{\"id\":" << presets[i] << ",\"name\":" << quoted(s.text(MDR_TEXT_EQUALIZER_PRESET_NAME,i)) << "}";
        std::cout << "]";
    }
    std::cout << ",\n  \"noise_change_verified\": " << (verified_noise ? "true":"false")
              << ",\n  \"original_noise_restored\": " << (restored ? "true":"false")
              << ",\n  \"eq_preset_bands_bass_verified\": " << (verified_eq ? "true":"false")
              << ",\n  \"original_eq_restored\": " << (verified_eq ? "true":"false")
              << ",\n  \"packets_sent\": " << s.tx << ",\n  \"packets_received\": " << s.rx << "\n}\n";
}

int main(int argc, char** argv) {
    bool ble=false, v1=false, verify=false, test_eq=false, events=false, survey=false;
    for (int i=1; i<argc; ++i) {
        std::string arg=argv[i];
        if (arg == "--ble") ble=true;
        else if (arg == "--v1") v1=true;
        else if (arg == "--verify-noise") verify=true;
        else if (arg == "--verify-eq") test_eq=true;
        else if(arg=="--events")events=true;
        else if(arg=="--survey-eq")survey=true;
        else if (arg == "--help") {
            std::cout << "earbud-probe [--ble | --v1] [--events] [--verify-noise] [--verify-eq] [--survey-eq]\nDefault: read-only V2 Classic probe; never scans or retries.\n"
                         "--verify-noise changes noise mode once, confirms it, then restores the original state.\n"
                         "--verify-eq tests preset selection, one EQ band, and Clear Bass, then restores the original EQ.\n"
                         "--survey-eq selects each preset to read its curve and Clear Bass, checks whether Clear Bass is per slot, then restores.\n";
            return 0;
        } else { std::cerr << "Unknown argument. Use --help.\n"; return 2; }
    }
    if (ble && v1) { std::cerr << "BLE and V1 are mutually exclusive.\n"; return 2; }
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        Session session;
        session.connect(find_earbuds(),ble,v1,events);
        bool verified=false, restored=false;
        if (verify) {
            if (v1) throw std::runtime_error("Automatic noise verification is limited to protocol V2.");
            if (session.feature(MDR_FEATURE_NOISE_CANCELLING) != MDR_AVAILABILITY_AVAILABLE)
                throw std::runtime_error("Noise cancelling is not advertised by the device.");
            MDRNoiseControl original{};
            check(mdrHeadphonesGetNoiseControl(session.headphones,MDR_PROPERTY_CURRENT,&original), "Read original noise state");
            auto alternate=original;
            alternate.mode=original.mode == MDR_NOISE_MODE_CANCELLING ? MDR_NOISE_MODE_OFF : MDR_NOISE_MODE_CANCELLING;
            try {
                session.write_noise(alternate);
                verified=true;
                std::this_thread::sleep_for(2s);
            } catch (...) {
                try { session.write_noise(original); } catch (...) {
                    std::cerr << "Original noise state could not be confirmed. Check it in Sony Sound Connect.\n";
                }
                throw;
            }
            try { session.write_noise(original); }
            catch (...) { std::cerr << "Noise test changed the mode but restoration was not confirmed. Check Sony Sound Connect.\n"; throw; }
            restored=true;
        }
        if(survey){survey_eq(session);return 0;}
        if(test_eq)verify_eq(session);
        snapshot(session,verified,restored,test_eq);
        return 0;
    } catch (const winrt::hresult_error& error) {
        std::cout << "{\"result\":\"failed\",\"error\":" << quoted(winrt::to_string(error.message())) << "}\n";
        return 1;
    } catch (const std::exception& error) {
        std::cout << "{\"result\":\"failed\",\"error\":" << quoted(error.what()) << "}\n";
        return 1;
    }
}


