#pragma once
#include "device.hpp"
#include <array>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <algorithm>
#include <map>
#include <optional>

struct Snapshot {
    bool ready{}, pending{}, noise_supported{}, ambient_supported{}, eq_supported{}, fresh{};
    std::string status="Not connected", error, firmware, address;
    std::array<int,3> batteries{-1,-1,-1};
    int noise=-1, ambient=-1, voice=-1, bass{};
    uint32_t preset{};
    std::vector<int32_t> bands;
    std::vector<std::pair<uint32_t,std::string>> presets;
    int64_t checked{};
    // Each preset's five bands and Clear Bass, as last read from the earbuds. Clear Bass belongs to
    // each preset and slot (surveyed on WF-C710N, 3 Nov 2026), so it is stored with the bands.
    std::map<uint32_t,std::pair<std::vector<int32_t>,int>> curves;
    bool release_when_hidden{};
    uint32_t connection_generation{};
    // Device settings, read back after every change. A key is present only when the earbuds
    // advertise that setting, so the app never shows a control that can't work.
    //   dsee 0/1, priority 1 sound / 2 stable, guidance 0/1, guidance_volume -2..2, auto_pause 0/1,
    //   wear_power 0/1, shutdown 1, cycle 1..4 (MDRNoiseButtonMode), touch_left / touch_right
    //   (MDRAssignableAction), general_<index> 0/1.
    std::map<std::string,int> settings;
    std::map<std::string,std::vector<int>> setting_options; // touch_left / touch_right choices
    std::map<std::string,std::string> setting_names;        // general_<index> subjects
    std::vector<std::string> devices;                       // connected devices, by name
};

inline const char* preset_name(uint32_t preset) {
    switch(preset) {
    case MDR_EQ_OFF:return "Off"; case MDR_EQ_BRIGHT:return "Bright"; case MDR_EQ_EXCITED:return "Excited";
    case MDR_EQ_MELLOW:return "Mellow"; case MDR_EQ_RELAXED:return "Relaxed"; case MDR_EQ_VOCAL:return "Vocal";
    case MDR_EQ_TREBLE:return "Treble boost"; case MDR_EQ_BASS:return "Bass boost"; case MDR_EQ_SPEECH:return "Speech";
    case MDR_EQ_CUSTOM:return "Manual"; case MDR_EQ_USER_1:return "Custom 1"; case MDR_EQ_USER_2:return "Custom 2";
    default:return "Device preset";
    }
}

// ApplyProfile writes five bands and Clear Bass to Manual and selects it: Manual is the app's hidden
// live slot. KeepSlot writes them to Custom 1 or 2 (value) and returns to the preset that was
// playing. ReadSlots reads Manual, Custom 1 and Custom 2 into curves for the first-connect import.
// Setting writes one advertised key from Snapshot::settings; PowerOff turns the earbuds off.
enum class Operation { Activate, Deactivate, Retry, Noise, Ambient, Voice, Preset, Bands, Bass, Reset, Policy, Suspend, Reconnect, Disconnect, ApplyProfile, KeepSlot, ReadSlots, Setting, PowerOff };
struct Command { Operation operation; int value{}; std::vector<int32_t> bands; int bass{}; uint32_t preset{}; std::string key; };

class Controller {
public:
    explicit Controller(std::function<void()> notify, Snapshot initial={}) : state_(std::move(initial)), notify_(std::move(notify)), worker_([this]{run();}) {}
    ~Controller() { {std::lock_guard lock(mutex_); stopping_=true;} SetEvent(wake_); worker_.join();CloseHandle(wake_); }
    Snapshot snapshot() const { std::lock_guard lock(mutex_); return state_; }
    void interest(bool active) {
        std::lock_guard lock(mutex_);
        if (requested_active_==active) return;
        requested_active_=active;
        commands_.push_back({active?Operation::Activate:Operation::Deactivate});
        SetEvent(wake_);
    }
    void lifecycle(Operation operation){std::lock_guard lock(mutex_);if(std::none_of(commands_.begin(),commands_.end(),[&](auto& c){return c.operation==operation;}))commands_.push_back({operation});SetEvent(wake_);}
    void release_when_hidden(bool release){
        {std::lock_guard lock(mutex_);state_.release_when_hidden=release;commands_.push_back({Operation::Policy});SetEvent(wake_);}
        notify_();
    }
    bool send(Command command) {
        {
            std::lock_guard lock(mutex_);
            if(command.operation==Operation::Retry && (!requested_active_||state_.pending)) return false;
            // Quick controls queue behind a write in progress, and only the newest of each is kept,
            // so a fast run of taps ends on the last one instead of being refused.
            const bool queues=command.operation==Operation::Noise||command.operation==Operation::Ambient||command.operation==Operation::Voice||command.operation==Operation::Setting;
            if(command.operation!=Operation::Retry && (!requested_active_ || !state_.ready || (!state_.fresh&&!queues)||(state_.pending&&!queues))) return false;
            if(command.operation==Operation::Bands || command.operation==Operation::Bass || command.operation==Operation::Ambient || command.operation==Operation::ApplyProfile||queues)
                std::erase_if(commands_,[&](const auto& c){return c.operation==command.operation&&c.key==command.key;});
            if(commands_.size()>=16) return false;
            commands_.push_back(std::move(command));
            state_.pending=true;
        }
        notify_();
        SetEvent(wake_);return true;
    }
private:
    mutable std::mutex mutex_;
    static HANDLE make_wake(){auto event=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!event)winrt::throw_last_error();return event;}
    HANDLE wake_=make_wake();
    Snapshot state_;
    std::function<void()> notify_;
    std::deque<Command> commands_;
    bool requested_active_{}, stopping_{};
    std::thread worker_;

    void publish(const std::function<void(Snapshot&)>& change) {
        {std::lock_guard lock(mutex_); change(state_);}
        notify_();
    }
    void read(Session& session) {
        auto next=snapshot();
        next.ready=true; next.fresh=true; next.error.clear(); next.status="Connected";
        next.firmware=session.text(MDR_TEXT_FIRMWARE_VERSION);
        next.noise_supported=session.noise_reported && session.feature(MDR_FEATURE_NOISE_CANCELLING)==MDR_AVAILABILITY_AVAILABLE;
        next.ambient_supported=session.noise_reported && session.feature(MDR_FEATURE_AMBIENT_SOUND)==MDR_AVAILABILITY_AVAILABLE;
        next.eq_supported=session.eq_reported && session.feature(MDR_FEATURE_EQUALIZER)==MDR_AVAILABILITY_AVAILABLE;
        if(session.battery_reported) {
            MDRBattery batteries[4]{}; uint32_t count=4;
            if(mdrHeadphonesGetBatteries(session.headphones,batteries,&count)==MDR_RESULT_OK) {
                next.batteries.fill(-1);
                for(uint32_t i=0;i<count;++i) {
                    int slot=batteries[i].part==MDR_BATTERY_LEFT?0:batteries[i].part==MDR_BATTERY_RIGHT?1:batteries[i].part==MDR_BATTERY_CASE?2:-1;
                    // The earbuds report 0 for a bud that isn't connected and for the case once
                    // both buds are out of it, so 0 means "not reported", never an empty battery.
                    if(slot>=0 && batteries[i].present && batteries[i].level_percent>0 && batteries[i].level_percent<=100) next.batteries[slot]=static_cast<int>(batteries[i].level_percent);
                }
            }
        }
        MDRNoiseControl noise{};
        if(session.noise_reported && mdrHeadphonesGetNoiseControl(session.headphones,MDR_PROPERTY_CURRENT,&noise)==MDR_RESULT_OK) {
            next.noise=static_cast<int>(noise.mode); next.ambient=static_cast<int>(noise.ambient_level); next.voice=noise.focus_on_voice?1:0;
        }
        if(next.eq_supported) {
            MDREqualizer eq{};
            check(mdrHeadphonesGetEqualizer(session.headphones,MDR_PROPERTY_CURRENT,&eq),"Read EQ");
            next.preset=eq.preset; next.bass=eq.clear_bass;
            int32_t bands[10]{};uint32_t count=10;
            check(mdrHeadphonesGetEqualizerBands(session.headphones,MDR_PROPERTY_CURRENT,bands,&count),"Read EQ bands");
            next.bands.assign(bands,bands+count);
            next.curves[next.preset]={next.bands,next.bass};
            MDREqualizerPreset presets[64]{};count=64;next.presets.clear();
            if(mdrHeadphonesGetEqualizerPresets(session.headphones,presets,&count)==MDR_RESULT_OK)
                for(uint32_t i=0;i<count;++i) {
                    if(presets[i]==MDR_EQ_UNKNOWN) continue;
                    auto name=session.text(MDR_TEXT_EQUALIZER_PRESET_NAME,i);
                    next.presets.emplace_back(presets[i],name.empty()?preset_name(presets[i]):name);
                }
        }
        read_settings(session,next);
        next.checked=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        publish([&](Snapshot& state){next.pending=state.pending;state=std::move(next);});
    }
    static bool offered(Session& session,MDRFeature feature){return session.feature(feature)==MDR_AVAILABILITY_AVAILABLE;}
    static std::optional<MDRAssignableControl> touch_key(Session& session,MDRAssignableActionKeyLocation location){
        MDRAssignableControl controls[8]{};uint32_t count=8;
        if(mdrHeadphonesGetAssignableControls(session.headphones,MDR_PROPERTY_CURRENT,controls,&count)!=MDR_RESULT_OK)return std::nullopt;
        for(uint32_t i=0;i<count&&i<8;++i)if(controls[i].location==location)return controls[i];
        return std::nullopt;
    }
    void read_settings(Session& session,Snapshot& next){
        next.settings.clear();next.setting_options.clear();next.setting_names.clear();next.devices.clear();
        auto& s=next.settings;
        MDREqualizer eq{};
        if(offered(session,MDR_FEATURE_DSEE)&&mdrHeadphonesGetEqualizer(session.headphones,MDR_PROPERTY_CURRENT,&eq)==MDR_RESULT_OK&&eq.dsee_available)s["dsee"]=eq.dsee_enabled?1:0;
        MDRConnectionMode mode{};
        if(offered(session,MDR_FEATURE_CONNECTION_MODE)&&mdrHeadphonesGetConnectionMode(session.headphones,MDR_PROPERTY_CURRENT,&mode)==MDR_RESULT_OK&&mode.audio_priority!=MDR_AUDIO_PRIORITY_UNKNOWN)s["priority"]=static_cast<int>(mode.audio_priority);
        MDRVoiceGuidance guidance{};
        if(offered(session,MDR_FEATURE_VOICE_GUIDANCE)&&mdrHeadphonesGetVoiceGuidance(session.headphones,MDR_PROPERTY_CURRENT,&guidance)==MDR_RESULT_OK){
            s["guidance"]=guidance.enabled?1:0;
            if(offered(session,MDR_FEATURE_VOICE_GUIDANCE_VOLUME))s["guidance_volume"]=std::clamp(guidance.volume,-2,2);
        }
        MDRPower power{};
        if(mdrHeadphonesGetPower(session.headphones,MDR_PROPERTY_CURRENT,&power)==MDR_RESULT_OK){
            if(offered(session,MDR_FEATURE_AUTO_PAUSE))s["auto_pause"]=power.auto_pause?1:0;
            if(power.wearing_power!=MDR_WEARING_POWER_UNAVAILABLE)s["wear_power"]=power.wearing_power==MDR_WEARING_POWER_WHEN_REMOVED?1:0;
        }
        if(offered(session,MDR_FEATURE_SHUTDOWN))s["shutdown"]=1;
        MDRNoiseControl noise{};
        if(session.noise_reported&&offered(session,MDR_FEATURE_NOISE_CONTROL_BUTTON)&&mdrHeadphonesGetNoiseControl(session.headphones,MDR_PROPERTY_CURRENT,&noise)==MDR_RESULT_OK
            &&noise.button_mode>=MDR_NOISE_BUTTON_NOISE_AMBIENT_OFF&&noise.button_mode<=MDR_NOISE_BUTTON_AMBIENT_OFF)s["cycle"]=static_cast<int>(noise.button_mode);
        if(offered(session,MDR_FEATURE_ASSIGNABLE_CONTROLS))
            for(auto [key,location]:{std::pair{"touch_left",MDR_ASSIGNABLE_ACTION_KEY_LEFT},std::pair{"touch_right",MDR_ASSIGNABLE_ACTION_KEY_RIGHT}}){
                auto control=touch_key(session,location);if(!control)continue;
                MDRAssignableAction actions[32]{};uint32_t count=32;
                if(mdrHeadphonesGetAssignableControlActions(session.headphones,location,actions,&count)!=MDR_RESULT_OK||count<2)continue;
                std::vector<int> options;for(uint32_t i=0;i<count&&i<32;++i)options.push_back(static_cast<int>(actions[i]));
                if(std::find(options.begin(),options.end(),static_cast<int>(control->action))==options.end())continue;
                s[key]=static_cast<int>(control->action);next.setting_options[key]=std::move(options);
            }
        if(offered(session,MDR_FEATURE_GENERAL_SETTINGS)){
            MDRGeneralSettingInfo info[4]{};uint32_t count=4;
            if(mdrHeadphonesGetGeneralSettingInfo(session.headphones,info,&count)==MDR_RESULT_OK)
                for(uint32_t i=0;i<count&&i<4;++i){
                    MDRGeneralSetting value{};auto subject=session.text(MDR_TEXT_GENERAL_SETTING_SUBJECT,info[i].index);
                    if(info[i].type!=MDR_GENERAL_SETTING_BOOLEAN||!info[i].writable||subject.empty())continue;
                    if(mdrHeadphonesGetGeneralSetting(session.headphones,MDR_PROPERTY_CURRENT,info[i].index,&value)!=MDR_RESULT_OK)continue;
                    auto key="general_"+std::to_string(info[i].index);s[key]=value.boolean_value?1:0;next.setting_names[key]=subject;
                }
        }
        if(offered(session,MDR_FEATURE_PAIRED_DEVICE_MANAGEMENT)){
            MDRPairedDevice devices[8]{};uint32_t count=8;
            if(mdrHeadphonesGetPairedDevices(session.headphones,devices,&count)==MDR_RESULT_OK)
                for(uint32_t i=0;i<count&&i<8;++i)if(devices[i].connected&&devices[i].name[0])next.devices.emplace_back(devices[i].name);
        }
    }
    // Device settings are committed, then given a moment to read back. Some (voice guidance on
    // WF-C710N) are only reported on the next sync, so a missing echo refreshes instead of failing:
    // the snapshot after the write shows the real value either way, and the controls stay up.
    template<class Confirmed> static void settle(Session& session,Confirmed confirmed){
        check(mdrHeadphonesRequestCommit(session.headphones),"Commit setting");
        session.until(MDR_EVENT_APPLY_COMPLETE,3s);
        for(auto deadline=Clock::now()+1500ms;Clock::now()<deadline;){session.poll();if(confirmed())return;}
        if(mdrHeadphonesIsReady(session.headphones)&&mdrHeadphonesRequestSync(session.headphones)==MDR_RESULT_OK){try{session.until(MDR_EVENT_SYNC_COMPLETE,3s);}catch(const NotConnected&){throw;}catch(...){}}
    }
    // Writes one advertised setting and reads it back.
    void write_setting(Session& session,const Command& command,const Snapshot& current){
        auto known=current.settings.find(command.key);
        if(known==current.settings.end())throw std::runtime_error("That setting isn't offered by these earbuds.");
        const auto& key=command.key;const int v=command.value;auto* h=session.headphones;
        auto flag=[](int value){if(value!=0&&value!=1)throw std::runtime_error("Invalid setting value.");return value?MDR_TRUE:MDR_FALSE;};
        if(key=="dsee"){
            check(mdrHeadphonesSetDSEE(h,flag(v)),"Set DSEE");
            settle(session,[&]{return (current_eq(session).dsee_enabled!=0)==(v!=0);});
        }else if(key=="priority"){
            if(v!=MDR_AUDIO_PRIORITY_QUALITY&&v!=MDR_AUDIO_PRIORITY_STABILITY)throw std::runtime_error("Invalid Bluetooth priority.");
            MDRConnectionMode mode{static_cast<MDRAudioPriority>(v)};
            check(mdrHeadphonesSetConnectionMode(h,&mode),"Set Bluetooth priority");
            // The earbuds may ask to confirm; the listener already chose this, so answer yes. They
            // may reconnect to apply it, which the Bluetooth connect event picks up.
            check(mdrHeadphonesRequestCommit(h),"Commit Bluetooth priority");
            bool answer=false;
            for(auto deadline=Clock::now()+6s;Clock::now()<deadline;){
                auto event=session.poll();
                if(event==MDR_EVENT_ALERT&&mdrHeadphonesSetAlertResponse(h,MDR_ALERT_ACTION_POSITIVE)==MDR_RESULT_OK)answer=true;
                if(answer&&mdrHeadphonesIsReady(h)){check(mdrHeadphonesRequestCommit(h),"Confirm Bluetooth priority");answer=false;}
                MDRConnectionMode now{};
                if(mdrHeadphonesGetConnectionMode(h,MDR_PROPERTY_CURRENT,&now)==MDR_RESULT_OK&&now.audio_priority==mode.audio_priority&&!answer)return;
            }
            throw std::runtime_error("Bluetooth priority wasn't confirmed by the earbuds.");
        }else if(key=="guidance"||key=="guidance_volume"){
            MDRVoiceGuidance g{};check(mdrHeadphonesGetVoiceGuidance(h,MDR_PROPERTY_CURRENT,&g),"Read voice guidance");
            if(key=="guidance")g.enabled=flag(v);else{if(v<-2||v>2)throw std::runtime_error("Invalid guidance volume.");g.volume=v;}
            check(mdrHeadphonesSetVoiceGuidance(h,&g),"Set voice guidance");
            settle(session,[&]{MDRVoiceGuidance now{};return mdrHeadphonesGetVoiceGuidance(h,MDR_PROPERTY_CURRENT,&now)==MDR_RESULT_OK&&(now.enabled!=0)==(g.enabled!=0)&&(key!="guidance_volume"||now.volume==g.volume);});
        }else if(key=="auto_pause"||key=="wear_power"){
            MDRPower p{};check(mdrHeadphonesGetPower(h,MDR_PROPERTY_CURRENT,&p),"Read power settings");p.shutdown_requested=MDR_FALSE;
            if(key=="auto_pause")p.auto_pause=flag(v);else p.wearing_power=flag(v)?MDR_WEARING_POWER_WHEN_REMOVED:MDR_WEARING_POWER_DISABLED;
            check(mdrHeadphonesSetPower(h,&p),"Set power settings");
            settle(session,[&]{MDRPower now{};return mdrHeadphonesGetPower(h,MDR_PROPERTY_CURRENT,&now)==MDR_RESULT_OK&&now.auto_pause==p.auto_pause&&now.wearing_power==p.wearing_power;});
        }else if(key=="cycle"){
            if(v<static_cast<int>(MDR_NOISE_BUTTON_NOISE_AMBIENT_OFF)||v>static_cast<int>(MDR_NOISE_BUTTON_AMBIENT_OFF))throw std::runtime_error("Invalid noise cycle.");
            MDRNoiseControl n{};check(mdrHeadphonesGetNoiseControl(h,MDR_PROPERTY_CURRENT,&n),"Read noise state");n.button_mode=static_cast<MDRNoiseButtonMode>(v);
            check(mdrHeadphonesSetNoiseControl(h,&n),"Set noise cycle");
            settle(session,[&]{MDRNoiseControl now{};return mdrHeadphonesGetNoiseControl(h,MDR_PROPERTY_CURRENT,&now)==MDR_RESULT_OK&&now.button_mode==n.button_mode;});
        }else if(key=="touch_left"||key=="touch_right"){
            auto options=current.setting_options.find(key);
            if(options==current.setting_options.end()||std::find(options->second.begin(),options->second.end(),v)==options->second.end())throw std::runtime_error("That touch action isn't offered.");
            MDRAssignableControl controls[8]{};uint32_t count=8;
            check(mdrHeadphonesGetAssignableControls(h,MDR_PROPERTY_CURRENT,controls,&count),"Read touch controls");
            const auto location=key=="touch_left"?MDR_ASSIGNABLE_ACTION_KEY_LEFT:MDR_ASSIGNABLE_ACTION_KEY_RIGHT;
            for(uint32_t i=0;i<count&&i<8;++i)if(controls[i].location==location)controls[i].action=static_cast<MDRAssignableAction>(v);
            check(mdrHeadphonesSetAssignableControls(h,controls,std::min<uint32_t>(count,8)),"Set touch controls");
            settle(session,[&]{auto now=touch_key(session,location);return now&&now->action==static_cast<MDRAssignableAction>(v);});
        }else if(key.starts_with("general_")){
            MDRGeneralSetting g{static_cast<uint32_t>(std::stoul(key.substr(8))),flag(v)};
            check(mdrHeadphonesSetGeneralSetting(h,&g),"Set setting");
            settle(session,[&]{MDRGeneralSetting now{};return mdrHeadphonesGetGeneralSetting(h,MDR_PROPERTY_CURRENT,g.index,&now)==MDR_RESULT_OK&&now.boolean_value==g.boolean_value;});
        }else throw std::runtime_error("Unsupported setting.");
    }
    void commit(Session& session,const std::function<bool()>& confirmed) {
        session.commit_confirmed(confirmed);
    }
    void execute(Session& session,const Command& command) {
        auto current=snapshot();
        if(command.operation==Operation::Setting){write_setting(session,command,current);return;}
        if(command.operation==Operation::PowerOff){
            if(!current.settings.count("shutdown"))throw std::runtime_error("These earbuds can't be turned off from here.");
            MDRPower p{};check(mdrHeadphonesGetPower(session.headphones,MDR_PROPERTY_CURRENT,&p),"Read power settings");
            p.shutdown_requested=MDR_TRUE;check(mdrHeadphonesSetPower(session.headphones,&p),"Turn earbuds off");
            check(mdrHeadphonesRequestCommit(session.headphones),"Turn earbuds off");
            try{session.until(MDR_EVENT_APPLY_COMPLETE,3s);}catch(...){} // they may drop the link before acknowledging
            throw NotConnected("The earbuds were turned off.");
        }
        if(command.operation==Operation::Noise || command.operation==Operation::Ambient || command.operation==Operation::Voice) {
            if(!current.noise_supported || (command.operation!=Operation::Noise && !current.ambient_supported)) throw std::runtime_error("This noise control is unavailable.");
            MDRNoiseControl wanted{};
            check(mdrHeadphonesGetNoiseControl(session.headphones,MDR_PROPERTY_CURRENT,&wanted),"Read noise state");
            if(command.operation==Operation::Noise) {
                if(command.value<0 || command.value>2) throw std::runtime_error("Invalid noise mode.");
                wanted.mode=static_cast<MDRNoiseMode>(command.value);
            } else if(command.operation==Operation::Voice) {
                wanted.focus_on_voice=command.value?MDR_TRUE:MDR_FALSE;
            } else {
                if(command.value<1 || command.value>20) throw std::runtime_error("Ambient level must be between 1 and 20.");
                wanted.mode=MDR_NOISE_MODE_AMBIENT; wanted.ambient_level=command.value;
            }
            session.write_noise(wanted);
            return;
        }
        if(!current.eq_supported) throw std::runtime_error("EQ is unavailable.");
        if(command.operation==Operation::ApplyProfile||command.operation==Operation::KeepSlot||command.operation==Operation::ReadSlots){write_slots(session,command,current);return;}
        if(command.operation!=Operation::Preset&&current.preset!=MDR_EQ_CUSTOM&&current.preset!=MDR_EQ_USER_1&&current.preset!=MDR_EQ_USER_2)throw std::runtime_error("Select Manual, Custom 1, or Custom 2 before editing.");
        if((command.operation==Operation::Bass&&(command.value < -10||command.value > 10))||std::any_of(command.bands.begin(),command.bands.end(),[](int value){return value < -10||value > 10;}))throw std::runtime_error("EQ values must be between -10 and +10.");
        if(command.operation==Operation::Preset) {
            if(std::none_of(current.presets.begin(),current.presets.end(),[&](const auto& p){return p.first==static_cast<uint32_t>(command.value);})) throw std::runtime_error("Preset is not advertised by the earbuds.");
            check(mdrHeadphonesSetEqualizerPreset(session.headphones,static_cast<uint32_t>(command.value)),"Set EQ preset");
        } else if(command.operation==Operation::Bass) {
            if(current.bands.size()!=5) throw std::runtime_error("Clear Bass is unavailable for this EQ layout.");
            check(mdrHeadphonesSetEqualizerClearBass(session.headphones,command.value),"Set Clear Bass");
        } else if(command.operation==Operation::Bands || command.operation==Operation::Reset) {
            if(command.bands.size()!=current.bands.size()) throw std::runtime_error("EQ band count changed; refresh before editing.");
            if(command.operation==Operation::Reset) {
                check(mdrHeadphonesSetEqualizerPreset(session.headphones,command.preset),"Restore EQ preset");
                check(mdrHeadphonesSetEqualizerClearBass(session.headphones,command.bass),"Restore Clear Bass");
            }
            check(mdrHeadphonesSetEqualizerBands(session.headphones,command.bands.data(),static_cast<uint32_t>(command.bands.size())),"Set EQ bands");
        } else throw std::runtime_error("Unsupported setting command.");
        commit(session,[&] {
            MDREqualizer eq{};
            if(mdrHeadphonesGetEqualizer(session.headphones,MDR_PROPERTY_CURRENT,&eq)!=MDR_RESULT_OK) return false;
            if(command.operation==Operation::Preset) return eq.preset==static_cast<uint32_t>(command.value);
            if(command.operation==Operation::Bass) return eq.clear_bass==command.value;
            int32_t bands[10]{};uint32_t count=10;
            if(mdrHeadphonesGetEqualizerBands(session.headphones,MDR_PROPERTY_CURRENT,bands,&count)!=MDR_RESULT_OK) return false;
            return std::vector<int32_t>(bands,bands+count)==command.bands && (command.operation!=Operation::Reset || (eq.preset==command.preset && eq.clear_bass==command.bass));
        });
    }
    static bool advertised(const Snapshot& state,uint32_t preset){return std::any_of(state.presets.begin(),state.presets.end(),[&](const auto& p){return p.first==preset;});}
    static MDREqualizer current_eq(Session& session){MDREqualizer eq{};check(mdrHeadphonesGetEqualizer(session.headphones,MDR_PROPERTY_CURRENT,&eq),"Read EQ");return eq;}
    static std::vector<int32_t> current_bands(Session& session){int32_t bands[10]{};uint32_t count=10;check(mdrHeadphonesGetEqualizerBands(session.headphones,MDR_PROPERTY_CURRENT,bands,&count),"Read EQ bands");return {bands,bands+count};}
    void select(Session& session,uint32_t preset){
        if(current_eq(session).preset==preset)return;
        check(mdrHeadphonesSetEqualizerPreset(session.headphones,preset),"Select EQ preset");
        session.commit_confirmed([&]{return current_eq(session).preset==preset;});
    }
    void write_curve(Session& session,const Command& command){
        check(mdrHeadphonesSetEqualizerBands(session.headphones,command.bands.data(),static_cast<uint32_t>(command.bands.size())),"Write EQ bands");
        check(mdrHeadphonesSetEqualizerClearBass(session.headphones,command.bass),"Write Clear Bass");
        // Success is every value reading back as written, not an acknowledged packet.
        session.commit_confirmed([&]{return current_eq(session).clear_bass==command.bass&&current_bands(session)==command.bands;});
    }
    void write_slots(Session& session,const Command& command,const Snapshot& current){
        if(command.operation==Operation::ReadSlots){
            const auto playing=current_eq(session).preset;
            try{for(uint32_t slot:{MDR_EQ_CUSTOM,MDR_EQ_USER_1,MDR_EQ_USER_2})if(advertised(current,slot)){select(session,slot);read(session);}}
            catch(...){try{select(session,playing);}catch(...){}throw;}
            select(session,playing);
            return;
        }
        if(command.bands.size()!=5||std::any_of(command.bands.begin(),command.bands.end(),[](int v){return v<-10||v>10;})||command.bass<-10||command.bass>10)throw std::runtime_error("EQ values must be five bands and Clear Bass between -10 and +10.");
        if(command.operation==Operation::ApplyProfile){
            if(!advertised(current,MDR_EQ_CUSTOM))throw std::runtime_error("These earbuds don't offer a Manual EQ slot.");
            select(session,MDR_EQ_CUSTOM);write_curve(session,command);
            return;
        }
        const auto slot=static_cast<uint32_t>(command.value);
        if((slot!=MDR_EQ_USER_1&&slot!=MDR_EQ_USER_2)||!advertised(current,slot))throw std::runtime_error("That earbud slot isn't available.");
        const auto playing=current_eq(session).preset;
        try{select(session,slot);write_curve(session,command);read(session);}
        catch(...){try{select(session,playing);}catch(...){}throw;} // never leave the listener on the slot being written
        select(session,playing);
    }
    void run() {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        std::unique_ptr<Session> session;
        bool active=false,suspended=false;
        auto release_at=Clock::time_point::max();
        // Earbuds often accept the Bluetooth link before their control channel
        // is ready, so a failed connect retries on its own with backoff.
        // Short first steps: the earbuds come back within seconds after a priority change or a
        // dropped link, and the first attempts usually find the control channel not quite ready.
        constexpr std::chrono::milliseconds retry_delays[]={1500ms,2s,3s,5s,10s,20s,30s,60s};
        auto retry_at=Clock::time_point::max();size_t retries=0;
        int link_grace=0; // rechecks after a connect event, before Windows marks the pair connected
        for(;;) {
            std::optional<Command> command;
            {
                std::unique_lock lock(mutex_);
                if(stopping_) break;
                if(!commands_.empty()){command=std::move(commands_.front());commands_.pop_front();}
                if(commands_.empty())ResetEvent(wake_);
            }
            bool scheduled=false,connecting=false;
            if(!command&&!session&&Clock::now()>=retry_at){scheduled=true;retry_at=Clock::time_point::max();command=Command{active?Operation::Retry:Operation::Reconnect};}
            // A fresh trigger (activation, Bluetooth link, resume) restarts the backoff.
            if(command&&!scheduled&&(command->operation==Operation::Activate||command->operation==Operation::Retry||command->operation==Operation::Reconnect)){retries=0;retry_at=Clock::time_point::max();link_grace=command->operation==Operation::Reconnect?2:0;}
            try {
                if(command && command->operation==Operation::Deactivate) {
                    active=false;release_at=snapshot().release_when_hidden?Clock::now()+15s:Clock::time_point::max();
                    if(!session) publish([](auto& s){s.ready=false;s.pending=false;s.fresh=false;s.status="Not connected";});
                } else if(command&&command->operation==Operation::Policy){release_at=!active&&snapshot().release_when_hidden?Clock::now()+15s:Clock::time_point::max();
                } else if(command&&(command->operation==Operation::Suspend||command->operation==Operation::Disconnect)){
                    suspended=command->operation==Operation::Suspend;session.reset();release_at=Clock::time_point::max();retry_at=Clock::time_point::max();retries=0;
                    publish([&](auto& s){s.ready=s.pending=s.fresh=false;s.status=suspended?"Sleeping":"Not connected";s.error.clear();});
                } else if(command && (command->operation==Operation::Activate || command->operation==Operation::Retry||command->operation==Operation::Reconnect)) {
                    if(command->operation==Operation::Reconnect){suspended=false;if(snapshot().address.empty()||(!active&&snapshot().release_when_hidden))continue;}
                    else {active=true;release_at=Clock::time_point::max();}
                    if(suspended)continue;
                    if(!session) {
                        connecting=true;session.reset();
                        // Check the Bluetooth link first so a disconnected pair never flashes Connecting.
                        auto device=find_earbuds();
                        publish([](auto& s){s.ready=false;s.fresh=false;s.pending=true;s.status="Connecting...";s.error.clear();});
                        auto previous=snapshot().address;
                        if(!previous.empty() && previous!=device.address) throw std::runtime_error("The remembered earbuds are unavailable; another WF-C710N will not be selected automatically.");
                        session=std::make_unique<Session>();session->connect(device,false,false,true);
                        publish([&](auto& s){s.address=device.address;++s.connection_generation;});
                        read(*session);retries=0;
                    }else if(command->operation!=Operation::Reconnect){
                        publish([](auto& s){s.pending=true;s.status="Refreshing...";});
                        check(mdrHeadphonesRequestSync(session->headphones),"Refresh state");session->until(MDR_EVENT_SYNC_COMPLETE,3s);read(*session);
                    }
                } else if(command) {
                    if(!session || !active) throw std::runtime_error("Controls are inactive. Open the widget and retry.");
                    execute(*session,*command);read(*session);
                }
                if(session && !active && Clock::now()>=release_at) {
                    session.reset();
                    publish([](auto& s){s.ready=false;s.fresh=false;s.status="Controls idle";});
                }
                if(session) {
                    for(int drained=0;drained<256;++drained){
                        auto event=session->poll();
                        if(event==MDR_EVENT_NEED_SYNC){
                            publish([](auto& s){s.fresh=false;});
                            check(mdrHeadphonesRequestSync(session->headphones),"Refresh changed state");
                            session->until(MDR_EVENT_SYNC_COMPLETE,3s);read(*session);
                        }else if(event==MDR_EVENT_NOISE_CONTROL_CHANGED||event==MDR_EVENT_EQUALIZER_CHANGED||event==MDR_EVENT_BATTERY_CHANGED||event==MDR_EVENT_GENERAL_SETTINGS_CHANGED||event==MDR_EVENT_ASSIGNABLE_CONTROLS_CHANGED||event==MDR_EVENT_POWER_CHANGED||event==MDR_EVENT_VOICE_GUIDANCE_CHANGED||event==MDR_EVENT_CONNECTION_MODE_CHANGED||event==MDR_EVENT_PAIRED_DEVICES_CHANGED)read(*session);
                        if(event==MDR_EVENT_NONE)break;
                        if(drained==255)throw std::runtime_error("Too many device events; controls paused to avoid continuous background work.");
                    }
                }
                bool pending;
                {std::lock_guard lock(mutex_);pending=!commands_.empty();}
                if(snapshot().pending!=pending) publish([&](auto& s){s.pending=pending;});
            } catch(const NotConnected&) {
                // The Bluetooth connect event wakes us with Reconnect; no polling until then.
                session.reset();
                if(link_grace>0&&retries<std::size(retry_delays)){--link_grace;retry_at=Clock::now()+retry_delays[retries++];}
                else{retry_at=Clock::time_point::max();retries=0;link_grace=0;}
                publish([](auto& s){s.ready=false;s.pending=false;s.fresh=false;s.status="Not connected";s.error.clear();});
            } catch(const std::exception& error) {
                session.reset();
                {std::lock_guard lock(mutex_);std::erase_if(commands_,[](const auto& c){return c.operation!=Operation::Activate && c.operation!=Operation::Deactivate;});}
                publish([&](auto& s){s.ready=false;s.pending=false;s.fresh=false;s.status=active?"Reconnecting…":"Controls unavailable";s.error=error.what();});
                // A drop mid-session retries too, not only a failed connect, and rides out the
                // moment Windows briefly reports the earbuds as disconnected.
                if((connecting||active)&&!suspended&&retries<std::size(retry_delays)){retry_at=Clock::now()+retry_delays[retries++];link_grace=std::max(link_grace,3);}
            } catch(const winrt::hresult_error& error) {
                session.reset();
                if((connecting||active)&&!suspended&&retries<std::size(retry_delays))retry_at=Clock::now()+retry_delays[retries++];
                publish([&](auto& s){s.ready=false;s.pending=false;s.fresh=false;s.status="Bluetooth unavailable";s.error=winrt::to_string(error.message());});
            }
            // No scheduled work while warm and idle. Only actual socket data,
            // commands, lifecycle events, or the optional release deadline wake us.
            HANDLE waits[]={wake_,session?session->wait_handle():nullptr};
            DWORD timeout=INFINITE;
            auto deadline=session?release_at:retry_at;
            if(deadline!=Clock::time_point::max())timeout=static_cast<DWORD>(std::clamp<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now()).count(),0,MAXDWORD-1));
            auto waited=WaitForMultipleObjects(waits[1]?2:1,waits,FALSE,timeout);
            if(waited==WAIT_FAILED){publish([](auto& s){s.ready=s.pending=s.fresh=false;s.status="Controls unavailable";s.error="Could not wait for Bluetooth events.";});break;}
        }
    }
};
