#include "controller.hpp"
#include <psapi.h>
#include <atomic>

static uint64_t process_cpu(){FILETIME created{},exited{},kernel{},user{};if(!GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user))winrt::throw_last_error();ULARGE_INTEGER k{},u{};k.LowPart=kernel.dwLowDateTime;k.HighPart=kernel.dwHighDateTime;u.LowPart=user.dwLowDateTime;u.HighPart=user.dwHighDateTime;return k.QuadPart+u.QuadPart;}
static PROCESS_MEMORY_COUNTERS_EX memory(){PROCESS_MEMORY_COUNTERS_EX value{};value.cb=sizeof(value);if(!GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&value),sizeof(value)))winrt::throw_last_error();return value;}
int main(){try{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    HANDLE changed=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!changed)winrt::throw_last_error();
    std::atomic<uint32_t> epoch{};
    {
        std::string address;const bandwave::ModelProfile* profile{};for(auto& device:bandwave::list_paired_sony())if(device.profile&&device.connected){address=device.address;profile=device.profile;break;}if(!profile)throw std::runtime_error("No supported Sony device is connected.");ControlSession controller(address,*profile,[&]{++epoch;SetEvent(changed);});
        auto wait=[&](uint32_t minimum){auto deadline=Clock::now()+12s;for(;;){auto s=controller.snapshot();if(epoch>=minimum&&s.ready&&!s.pending)return s;if(!s.error.empty())throw std::runtime_error(s.error);if(Clock::now()>=deadline)throw std::runtime_error("Read-only controller verification timed out.");WaitForSingleObject(changed,1000);}};
        controller.interest(true);auto initial=wait(4);
        controller.interest(false);Sleep(100);
        auto before=memory();DWORD handles_before{};GetProcessHandleCount(GetCurrentProcess(),&handles_before);
        auto start=Clock::now();auto cpu_before=process_cpu();
        Sleep(60000);
        double seconds=std::chrono::duration<double>(Clock::now()-start).count();auto cpu_after=process_cpu();auto after=memory();DWORD handles_after{};GetProcessHandleCount(GetCurrentProcess(),&handles_after);
        auto retained=controller.snapshot();
        auto next=epoch.load()+3;controller.interest(true);auto reopened=wait(next);
        for(int cycle=0;cycle<20;++cycle){controller.interest(false);Sleep(10);next=epoch.load()+3;controller.interest(true);reopened=wait(next);}
        auto final=memory();DWORD final_handles{};GetProcessHandleCount(GetCurrentProcess(),&final_handles);
        double cpu=(cpu_after-cpu_before)/10000000.0/seconds/GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)*100.0;
        std::cout<<"{\"seconds\":"<<seconds<<",\"retained\":"<<(retained.ready?"true":"false")<<",\"connection_generation\":"<<reopened.connection_generation<<",\"warm_controller_cycles\":20,\"private_mib_before\":"<<before.PrivateUsage/1048576.0<<",\"private_mib_after\":"<<after.PrivateUsage/1048576.0<<",\"working_set_mib\":"<<after.WorkingSetSize/1048576.0<<",\"cpu_machine_percent\":"<<cpu<<",\"handles_before\":"<<handles_before<<",\"handles_after\":"<<handles_after<<",\"handles_after_cycles\":"<<final_handles<<",\"private_mib_after_cycles\":"<<final.PrivateUsage/1048576.0<<",\"eq_unchanged\":"<<((initial.preset==reopened.preset&&initial.bass==reopened.bass&&initial.bands==reopened.bands)?"true":"false")<<"}\n";
        if(!retained.ready||reopened.connection_generation!=1||cpu>=0.1||after.PrivateUsage>30*1048576||initial.bands!=reopened.bands)throw std::runtime_error("Read-only retained-controller acceptance failed.");
    }
    CloseHandle(changed);return 0;
}catch(const winrt::hresult_error& e){std::cerr<<winrt::to_string(e.message())<<"\n";return 1;}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
