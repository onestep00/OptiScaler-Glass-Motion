// Scoped live diagnostic: augment the native cache-provider declaration result.
// Existing compiled layouts are not invalidated by this hook.
#ifdef GLASS_RED4EXT_STARTUP
#include <windows.h>
#include <RED4ext/Api/ApiVersion.hpp>
#include <RED4ext/Api/v1/EMainReason.hpp>
#include <RED4ext/Api/v1/PluginInfo.hpp>
#include <RED4ext/Api/v1/Runtime.hpp>
#include <RED4ext/Api/v1/Sdk.hpp>
#include <RED4ext/Api/v1/Version.hpp>
#else
#include "DetourThreads.h"
#endif
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
namespace {
using GetMetadata=int(*)(void*,uint64_t,void**);
GetMetadata original=nullptr;
HMODULE module=nullptr;
std::atomic<bool> enabled=false;
std::atomic<uint64_t> seen=0,matched=0,rejected=0;
std::mutex control;
std::mutex initialization;
std::atomic<bool> prepared=false;
alignas(16) std::array<unsigned char,32> expected{},augmented{},names{};
uint64_t wanted=0xccd5360c883c5323ULL;
bool installed=false;
bool manualVerifiedOnce=false;
std::filesystem::path output;
bool prepare(void* record);
int hook(void* provider,uint64_t key,void** result) {
    const auto value=original(provider,key,result);
    if(!enabled.load(std::memory_order_acquire))return value;
    ++seen;
    if(value!=1 || key!=wanted || !result || !*result)return value;
    if(!prepared.load(std::memory_order_acquire)) {
        std::lock_guard lock(initialization);
        if(!prepared.load(std::memory_order_relaxed) && !prepare(*result)){++rejected;return value;}
    }
    if(memcmp(*result,expected.data(),expected.size())){++rejected;return value;}
    *result=augmented.data();++matched;return value;
}
bool read(uint64_t address,void* out,size_t bytes) noexcept {
    __try {
        if(address<0x10000 || address>0x7fffffffffffULL-bytes)return false;
        memcpy(out,reinterpret_cast<void*>(address),bytes);return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
template<class T> bool get(uint64_t address,T& out) {return read(address,&out,sizeof(out));}
bool prepare(void* record) {
    if(!record || !read(reinterpret_cast<uint64_t>(record),expected.data(),32))return false;
    uint64_t source=0;uint32_t mask=0,count=0,capacity=0;
    memcpy(&wanted,expected.data(),8);memcpy(&mask,expected.data()+8,4);
    memcpy(&source,expected.data()+16,8);memcpy(&capacity,expected.data()+24,4);memcpy(&count,expected.data()+28,4);
    // This single metadata case is selected from current shader bytecode, not a runtime material whitelist.
    if(wanted!=0xccd5360c883c5323ULL || count!=1 || capacity<count || (mask&128) || !read(source,names.data(),16))return false;
    uint64_t existing=0;memcpy(&existing,names.data(),8);
    if(existing!=0xa2170d5019925575ULL || names[8]!=1)return false;
    const uint64_t motion=0x5fc6bea24112bd53ULL;memcpy(names.data()+16,&motion,8);names[24]=24;
    augmented=expected;mask|=128;memcpy(augmented.data()+8,&mask,4);
    source=reinterpret_cast<uint64_t>(names.data());memcpy(augmented.data()+16,&source,8);
    count=2;memcpy(augmented.data()+24,&count,4);memcpy(augmented.data()+28,&count,4);
    prepared.store(true,std::memory_order_release);return true;
}
bool profile(uint64_t base) {
    std::ifstream file(output.parent_path()/"profile.bin",std::ios::binary);
    uint32_t rva=0,bytes=0;file.read(reinterpret_cast<char*>(&rva),4);file.read(reinterpret_cast<char*>(&bytes),4);
    if(!file || rva!=0x2adc5c || !bytes || bytes>512)return false;
    std::array<unsigned char,512> a{},b{};
    return bool(file.read(reinterpret_cast<char*>(a.data()),bytes)) && read(base+rva,b.data(),bytes) &&
        !memcmp(a.data(),b.data(),bytes) && file.peek()==std::char_traits<char>::eof();
}
void save(bool manualVerified,uint64_t beforeSeen,uint64_t beforeMatched) {
    std::ofstream file(output/"status.json");
    file<<"{\"enabled\":"<<enabled.load()<<",\"seen\":"<<seen.load()<<",\"matched\":"<<matched.load()
        <<",\"rejected\":"<<rejected.load()<<",\"before_manual_seen\":"<<beforeSeen<<",\"before_manual_matched\":"<<beforeMatched
        <<",\"manual_native_provider_verified\":"<<(manualVerified || manualVerifiedOnce)
        <<",\"declaration_prepared\":"<<prepared.load()<<",\"cached_layout_invalidated\":false,\"fg_input_changed\":false}";
}
}
#ifndef GLASS_RED4EXT_STARTUP
extern "C" __declspec(dllexport) DWORD WINAPI MotionProbeStart(void* argument) {
    std::lock_guard lock(control);
    try {
        if(installed || !argument)return 1;
        output=static_cast<const wchar_t*>(argument);
        if(!output.is_absolute() || !std::filesystem::create_directory(output))return 2;
        const auto base=reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
        if(!profile(base))return 3;
        uint64_t renderer=0,provider=0,table=0,method=0;
        if(!get(base+0x3438990,renderer) || !get(renderer+16,provider) || !get(provider,table) ||
           !get(table+0x58,method) || method!=base+0x2adc5c)return 4;
        original=reinterpret_cast<GetMetadata>(method);void* record=nullptr;
        if(original(reinterpret_cast<void*>(provider),0xccd5360c883c5323ULL,&record)!=1 || !prepare(record))return 5;
        HMODULE pinned=nullptr;
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                             reinterpret_cast<LPCWSTR>(&MotionProbeStart),&pinned))return 6;
        // Same thread enlistment helper already exercised by the existing native hooks.
        GlassFg::DetourThreads threads;
        if(!threads.gather() || DetourTransactionBegin()!=NO_ERROR)return 7;
        if(DetourAttach(reinterpret_cast<PVOID*>(&original),hook)!=NO_ERROR || !threads.enlist()) {
            DetourTransactionAbort();return 8;
        }
        if(DetourTransactionCommit()!=NO_ERROR)return 9;
        installed=true;enabled.store(true,std::memory_order_release);
        const auto beforeSeen=seen.load(),beforeMatched=matched.load();void* returned=nullptr;
        const auto getter=reinterpret_cast<GetMetadata>(method);
        const bool verified=getter(reinterpret_cast<void*>(provider),wanted,&returned)==1 && returned==augmented.data() &&
                            !memcmp(record,expected.data(),32);
        manualVerifiedOnce=verified;save(verified,beforeSeen,beforeMatched);
        if(!verified){enabled.store(false);return 10;}
        return 0;
    } catch(...) {enabled.store(false);return 11;}
}
#else
extern "C" __declspec(dllexport) bool __fastcall Main(RED4ext::v1::PluginHandle handle,
    RED4ext::v1::EMainReason reason,const RED4ext::v1::Sdk* sdk) {
    std::lock_guard lock(control);
    try {
        if(!sdk || !sdk->hooking)return false;
        const auto base=reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
        if(reason==RED4ext::v1::EMainReason::Load) {
            wchar_t path[32768];if(!GetModuleFileNameW(module,path,32768))return false;
            output=std::filesystem::path(path).parent_path()/
                ("startup-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
            if(!std::filesystem::create_directory(output) || !profile(base))return false;
            // Game objects/allocators do not exist yet. Resolve declaration data
            // lazily from the provider's first original callback after startup.
            original=reinterpret_cast<GetMetadata>(base+0x2adc5c);
            if(!sdk->hooking->Attach(handle,reinterpret_cast<void*>(base+0x2adc5c),
                                    reinterpret_cast<void*>(&hook),reinterpret_cast<void**>(&original)))return false;
            installed=true;enabled.store(true,std::memory_order_release);save(false,0,0);
            if(sdk->logger)sdk->logger->Info(handle,"GlassMotion: declaration hook installed before material creation; scoped diagnostic, FG replacement pending.");
        } else if(reason==RED4ext::v1::EMainReason::Unload) {
            enabled.store(false,std::memory_order_release);
            if(installed && !sdk->hooking->Detach(handle,reinterpret_cast<void*>(base+0x2adc5c)))return false;
            installed=false;if(!output.empty())save(false,seen.load(),matched.load());
        }
        return true;
    } catch(...) {enabled.store(false);return false;}
}
extern "C" __declspec(dllexport) void __fastcall Query(RED4ext::v1::PluginInfo* info) {
    info->name=L"GlassMotion.Engine";info->author=L"onestep00";
    info->version=RED4EXT_V1_SEMVER(0,1,0);info->runtime=RED4EXT_V1_RUNTIME_VERSION_INDEPENDENT;
    info->sdk=RED4EXT_V1_SDK_VERSION_CURRENT;
}
extern "C" __declspec(dllexport) uint32_t __fastcall Supports() {return RED4EXT_API_VERSION_1;}
#endif
extern "C" __declspec(dllexport) DWORD WINAPI MotionProbeSave(void*) {
    std::lock_guard lock(control);save(false,seen.load(),matched.load());return 0;
}
extern "C" __declspec(dllexport) DWORD WINAPI MotionProbeStop(void*) {
    std::lock_guard lock(control);enabled.store(false);save(false,seen.load(),matched.load());return 0;
}
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,void*) {if(reason==DLL_PROCESS_ATTACH)module=instance;return TRUE;}
#ifdef GLASS_DECLARATION_SELFTEST
int main() {
    alignas(16) std::array<unsigned char,32> record{};
    alignas(16) std::array<unsigned char,16> inputNames{};
    uint64_t key=0xccd5360c883c5323ULL,name=0xa2170d5019925575ULL,pointer=reinterpret_cast<uint64_t>(inputNames.data());
    uint32_t mask=272642596,count=1;
    memcpy(record.data(),&key,8);memcpy(record.data()+8,&mask,4);memcpy(record.data()+16,&pointer,8);
    memcpy(record.data()+24,&count,4);memcpy(record.data()+28,&count,4);memcpy(inputNames.data(),&name,8);inputNames[8]=1;
    const auto before=record;const auto beforeNames=inputNames;
    if(!prepare(record.data()) || record!=before || inputNames!=beforeNames)return 1;
    original=+[](void* data,uint64_t,void** out){*out=data;return 1;};
    void* returned=nullptr;prepared=false;enabled=true;
    if(hook(record.data(),key,&returned)!=1 || returned!=augmented.data())return 2;
    uint32_t actualMask=0;memcpy(&actualMask,augmented.data()+8,4);
    if(actualMask!=(mask|128) || names[24]!=24 || memcmp(names.data(),inputNames.data(),16))return 3;
    enabled=false;hook(record.data(),key,&returned);
    if(returned!=record.data() || record!=before || inputNames!=beforeNames)return 4;
    return 0;
}
#endif
