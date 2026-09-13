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
#include <cstdio>
#include <intrin.h>
#include "MotionDeclarationTable.h"
#include "MotionShaderScope.h"
#include "CyberpunkDeclarationProfile.h"
namespace {
using GetMetadata=int(*)(void*,uint64_t,void**);
GetMetadata original=nullptr;
using ResolveStage=bool(*)(void*,uint8_t,void*,void**,uint32_t*,void*);
ResolveStage originalStage=nullptr;
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
GlassFg::MotionDeclarationTable declarationTable;
GlassFg::MotionShaderScope shaderScope;
std::atomic<uint64_t> stageSeen=0,stageSelected=0,outsideScope=0;
std::atomic<uint32_t> preparedDeclarations=0;
std::atomic<uint64_t> revalidatedDeclarations=0;
uint64_t providerEntry=0;
uint64_t stageEntry=0;
bool stageInstalled=false;
bool nativeRoutesResolved=false;
const char* startupStatus="not_started";
bool prepare(void* record);
bool read(uint64_t address,void* out,size_t bytes) noexcept;
int metadataCall(void* provider,uint64_t key,void** result,uint64_t caller) {
    const auto value=original(provider,key,result);
    if(!enabled.load(std::memory_order_acquire))return value;
    ++seen;
    if(declarationTable.count()) {
        if(value!=1 || !result || !*result)return value;
        if(!GlassFg::MotionShaderScope::Invocation::allows(key,caller)){++outsideScope;return value;}
        const auto rewritten=declarationTable.rewrite(key,*result,read);
        using State=GlassFg::MotionDeclarationTable::State;
        if(rewritten.metadata) {
            *result=const_cast<GlassFg::MotionDeclarationTable::Metadata*>(rewritten.metadata);
            ++matched;
            if(rewritten.state==State::Prepared)++preparedDeclarations;
            if(rewritten.state==State::Revalidated)++revalidatedDeclarations;
        } else if(rewritten.state!=State::NotSelected)++rejected;
        return value;
    }
    if(value!=1 || key!=wanted || !result || !*result)return value;
    if(!prepared.load(std::memory_order_acquire)) {
        std::lock_guard lock(initialization);
        if(!prepared.load(std::memory_order_relaxed) && !prepare(*result)){++rejected;return value;}
    }
    if(memcmp(*result,expected.data(),expected.size())){++rejected;return value;}
    *result=augmented.data();++matched;return value;
}
int hook(void* provider,uint64_t key,void** result) {
    return metadataCall(provider,key,result,reinterpret_cast<uint64_t>(_ReturnAddress()));
}
bool stageHook(void* combination,uint8_t stage,void* unused,void** shader,uint32_t* mask,void* mergedNames) {
    uint64_t key=0;
    if(enabled.load(std::memory_order_acquire) && shaderScope.count()) {
        ++stageSeen;
        std::array<uint64_t,3> record{};
        // Original stage 0 resolves combination+8 (VS); stage 1 resolves +16
        // (PS). The loaded cache confirms this ordering for every selected pair.
        if(stage==0 && read(reinterpret_cast<uint64_t>(combination),record.data(),sizeof(record))) {
            key=shaderScope.lookup(record[1],record[2]);
            if(key)++stageSelected;
        }
    }
    GlassFg::MotionShaderScope::Invocation scope(key,stageEntry+GlassFg::CyberpunkDeclarations::StageMetadataReturn);
    return originalStage(combination,stage,unused,shader,mask,mergedNames);
}
bool read(uint64_t address,void* out,size_t bytes) noexcept {
    __try {
        if(address<0x10000 || address>0x7fffffffffffULL-bytes)return false;
        memcpy(out,reinterpret_cast<void*>(address),bytes);return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
template<class T> bool get(uint64_t address,T& out) {return read(address,&out,sizeof(out));}
bool loadDeclarations() {
    const auto path=output.parent_path()/"declarations.bin";
    if(!std::filesystem::exists(path))return true; // Preserve the preceding scoped diagnostic when absent.
    std::ifstream file(path,std::ios::binary);
    std::array<char,8> magic{};uint32_t count=0,nameCount=0;
    file.read(magic.data(),magic.size());file.read(reinterpret_cast<char*>(&count),4);
    file.read(reinterpret_cast<char*>(&nameCount),4);
    using Table=GlassFg::MotionDeclarationTable;
    if(!file || memcmp(magic.data(),"GMDPLAN1",8) || !count || count>Table::MaxDeclarations ||
       nameCount>Table::MaxNames)return false;
    std::array<Table::Plan,Table::MaxDeclarations> plans{};
    std::array<Table::Name,Table::MaxNames> declaredNames{};
    file.read(reinterpret_cast<char*>(plans.data()),count*sizeof(Table::Plan));
    file.read(reinterpret_cast<char*>(declaredNames.data()),nameCount*sizeof(Table::Name));
    if(!file || file.peek()!=std::char_traits<char>::eof())return false;
    if(!declarationTable.configure({plans.data(),count},{declaredNames.data(),nameCount}))return false;
    std::ifstream pairsFile(output.parent_path()/"shader-pairs.bin",std::ios::binary);
    uint32_t pairCount=0,reserved=0;
    pairsFile.read(magic.data(),8);pairsFile.read(reinterpret_cast<char*>(&pairCount),4);
    pairsFile.read(reinterpret_cast<char*>(&reserved),4);
    using Scope=GlassFg::MotionShaderScope;
    if(!pairsFile || memcmp(magic.data(),"GMSPAIR1",8) || reserved || !pairCount || pairCount>Scope::MaxPairs)return false;
    std::array<Scope::Pair,Scope::MaxPairs> pairs{};
    pairsFile.read(reinterpret_cast<char*>(pairs.data()),pairCount*sizeof(Scope::Pair));
    if(!pairsFile || pairsFile.peek()!=std::char_traits<char>::eof())return false;
    for(uint32_t i=0;i<pairCount;++i) {
        bool found=false;
        for(uint32_t j=0;j<count;++j)if(plans[j].key==pairs[i].metadata){found=true;break;}
        if(!found)return false;
    }
    return shaderScope.configure({pairs.data(),pairCount});
}
bool resolveNativeRoutes(uint64_t base) {
    IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};
    if(!get(base,dos) || dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<0 || dos.e_lfanew>4096 ||
       !get(base+dos.e_lfanew,nt) || nt.Signature!=IMAGE_NT_SIGNATURE ||
       nt.OptionalHeader.SizeOfImage<4096 || nt.OptionalHeader.SizeOfImage>1024u*1024u*1024u)return false;
    GlassFg::RelocatableCode image;
    GlassFg::CyberpunkDeclarations::Layout layout;
    if(!image.initialize({reinterpret_cast<const unsigned char*>(base),nt.OptionalHeader.SizeOfImage}) ||
       !layout.resolve(image))return false;
    providerEntry=base+layout.metadata;stageEntry=base+layout.stage;nativeRoutesResolved=true;return true;
}
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
        <<",\"declaration_prepared\":"<<(prepared.load() || preparedDeclarations.load()!=0)
        <<",\"configured_declarations\":"<<declarationTable.count()
        <<",\"prepared_declarations\":"<<preparedDeclarations.load()
        <<",\"revalidated_declarations\":"<<revalidatedDeclarations.load()
        <<",\"declaration_table_bytes\":"<<sizeof(declarationTable)
        <<",\"native_routes_resolved\":"<<nativeRoutesResolved
        <<",\"configured_shader_pairs\":"<<shaderScope.count()
        <<",\"stage_hook_installed\":"<<stageInstalled
        <<",\"stage_seen\":"<<stageSeen.load()<<",\"stage_selected\":"<<stageSelected.load()
        <<",\"outside_shader_scope\":"<<outsideScope.load()
        <<",\"startup_status\":\""<<startupStatus<<"\""
        <<",\"cached_layout_invalidated\":false,\"fg_input_changed\":false}";
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
            if(!std::filesystem::create_directory(output))return false;
            if(!resolveNativeRoutes(base)) {
                startupStatus="native_layout_rejected";save(false,0,0);
                if(sdk->logger)sdk->logger->Info(handle,"GlassMotion: native declaration route validation failed; hook not installed.");
                return false;
            }
            if(!loadDeclarations()) {
                startupStatus="declaration_profile_rejected";save(false,0,0);
                if(sdk->logger)sdk->logger->Info(handle,"GlassMotion: declaration table validation failed; hook not installed.");
                return false;
            }
            // Game objects/allocators do not exist yet. Resolve declaration data
            // lazily from the provider's first original callback after startup.
            // Keep callback/immutable declaration storage valid even if a later
            // Attach fails and the loader rejects this plugin. Stop still turns
            // all redirection off; this small startup adapter lasts until exit.
            HMODULE pinned=nullptr;
            if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                                  reinterpret_cast<LPCWSTR>(&Main),&pinned)) {
                startupStatus="module_pin_failed";save(false,0,0);return false;
            }
            original=reinterpret_cast<GetMetadata>(providerEntry);
            if(!sdk->hooking->Attach(handle,reinterpret_cast<void*>(providerEntry),
                                    reinterpret_cast<void*>(&hook),reinterpret_cast<void**>(&original))) {
                startupStatus="hook_attach_failed";save(false,0,0);return false;
            }
            installed=true;
            if(shaderScope.count()) {
                originalStage=reinterpret_cast<ResolveStage>(stageEntry);
                if(!sdk->hooking->Attach(handle,reinterpret_cast<void*>(stageEntry),
                                        reinterpret_cast<void*>(&stageHook),reinterpret_cast<void**>(&originalStage))) {
                    startupStatus="stage_hook_attach_failed";
                    if(sdk->hooking->Detach(handle,reinterpret_cast<void*>(providerEntry)))installed=false;
                    save(false,0,0);return false;
                }
                stageInstalled=true;
            }
            startupStatus="running_diagnostic";
            enabled.store(true,std::memory_order_release);save(false,0,0);
            if(sdk->logger)sdk->logger->Info(handle,"GlassMotion: declaration hook installed before material creation; diagnostic, FG replacement pending.");
        } else if(reason==RED4ext::v1::EMainReason::Unload) {
            enabled.store(false,std::memory_order_release);
            if(stageInstalled && !sdk->hooking->Detach(handle,reinterpret_cast<void*>(stageEntry)))return false;
            stageInstalled=false;
            if(installed && !sdk->hooking->Detach(handle,reinterpret_cast<void*>(providerEntry)))return false;
            installed=false;startupStatus="stopped";if(!output.empty())save(false,seen.load(),matched.load());
        }
        return true;
    } catch(...) {enabled.store(false);return false;}
}
extern "C" __declspec(dllexport) void __fastcall Query(RED4ext::v1::PluginInfo* info) {
    info->name=L"GlassMotion.Engine";info->author=L"onestep00";
    info->version=RED4EXT_V1_SEMVER(0,2,0);info->runtime=RED4EXT_V1_RUNTIME_VERSION_INDEPENDENT;
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
int wmain(int argc,wchar_t** argv) {
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
    if(argc==2) {
        // The optional cache-derived fixture exercises the same file loader and
        // callback on owned CPU records. It never opens or calls a game process.
        output=std::filesystem::path(argv[1])/"owned-unused";
        if(!loadDeclarations() || !declarationTable.count())return 5;
        using Table=GlassFg::MotionDeclarationTable;
        std::ifstream input(output.parent_path()/"declarations.bin",std::ios::binary);
        std::array<char,8> magic{};uint32_t fixtureCount=0,nameCount=0;
        input.read(magic.data(),8);input.read(reinterpret_cast<char*>(&fixtureCount),4);
        input.read(reinterpret_cast<char*>(&nameCount),4);
        if(!input || fixtureCount>Table::MaxDeclarations || nameCount>Table::MaxNames)return 6;
        std::array<Table::Plan,Table::MaxDeclarations> plans{};
        std::array<Table::Name,Table::MaxNames> fixtureNames{};
        input.read(reinterpret_cast<char*>(plans.data()),fixtureCount*sizeof(Table::Plan));
        input.read(reinterpret_cast<char*>(fixtureNames.data()),nameCount*sizeof(Table::Name));
        if(!input)return 7;
        enabled=true;
        for(uint32_t i=0;i<fixtureCount;++i) {
            const auto& plan=plans[i];
            Table::Metadata sourceRecord{plan.key,plan.mask,0,
                reinterpret_cast<uint64_t>(&fixtureNames[plan.nameOffset]),plan.nameCount,plan.nameCount};
            const auto unchanged=sourceRecord;
            void* actual=nullptr;
            GlassFg::MotionShaderScope::Invocation fixtureScope(plan.key,stageEntry+GlassFg::CyberpunkDeclarations::StageMetadataReturn);
            if(metadataCall(&sourceRecord,plan.key,&actual,stageEntry+GlassFg::CyberpunkDeclarations::StageMetadataReturn)!=1 || !actual || actual==&sourceRecord ||
               memcmp(&sourceRecord,&unchanged,sizeof(unchanged)))return 8;
            const auto& modified=*static_cast<Table::Metadata*>(actual);
            if(modified.key!=plan.key || modified.mask!=(plan.mask|128) || modified.count!=uint32_t(plan.nameCount)+1)return 9;
            const auto* actualNames=reinterpret_cast<const Table::Name*>(modified.names);
            if(actualNames[plan.nameCount].hash!=Table::MotionName || actualNames[plan.nameCount].index!=plan.motionRow)return 10;
        }
        using Scope=GlassFg::MotionShaderScope;
        std::ifstream pairInput(output.parent_path()/"shader-pairs.bin",std::ios::binary);
        uint32_t fixturePairs=0,reserved=0;
        pairInput.read(magic.data(),8);pairInput.read(reinterpret_cast<char*>(&fixturePairs),4);
        pairInput.read(reinterpret_cast<char*>(&reserved),4);
        std::array<Scope::Pair,Scope::MaxPairs> pairs{};
        if(fixturePairs>Scope::MaxPairs || !pairInput.read(reinterpret_cast<char*>(pairs.data()),fixturePairs*sizeof(Scope::Pair)))return 11;
        originalStage=+[](void*,uint8_t,void* data,void** result,uint32_t*,void*) {
            const auto* sourceRecord=static_cast<Table::Metadata*>(data);
            return metadataCall(data,sourceRecord->key,result,stageEntry+GlassFg::CyberpunkDeclarations::StageMetadataReturn)==1;
        };
        for(uint32_t i=0;i<fixturePairs;++i) {
            const auto& pair=pairs[i];
            const Table::Plan* plan=nullptr;
            for(uint32_t j=0;j<fixtureCount;++j)if(plans[j].key==pair.metadata){plan=&plans[j];break;}
            if(!plan)return 12;
            Table::Metadata sourceRecord{plan->key,plan->mask,0,
                reinterpret_cast<uint64_t>(&fixtureNames[plan->nameOffset]),plan->nameCount,plan->nameCount};
            const auto unchanged=sourceRecord;
            std::array<uint64_t,3> combination{0,pair.vertex,pair.partner};
            const auto unchangedCombination=combination;
            void* actual=nullptr;
            if(!stageHook(combination.data(),0,&sourceRecord,&actual,nullptr,nullptr) || actual==&sourceRecord)return 13;
            if(!stageHook(combination.data(),1,&sourceRecord,&actual,nullptr,nullptr) || actual!=&sourceRecord)return 14;
            // Same declaration outside the native stage callback must remain untouched.
            if(metadataCall(&sourceRecord,plan->key,&actual,stageEntry+GlassFg::CyberpunkDeclarations::StageMetadataReturn)!=1 || actual!=&sourceRecord)return 15;
            if(combination!=unchangedCombination || memcmp(&sourceRecord,&unchanged,sizeof(unchanged)))return 16;
        }
        enabled=false;
        std::printf("{\"configured\":%u,\"prepared\":%u,\"rejected\":%llu,\"stage_pairs\":%u,\"stage_selected\":%llu,"
                    "\"pixel_stage_and_unscoped_unchanged\":true,\"owned_callback\":true,\"game_attached\":false}\n",
            declarationTable.count(),preparedDeclarations.load(),static_cast<unsigned long long>(rejected.load()),
            fixturePairs,static_cast<unsigned long long>(stageSelected.load()));
    }
    return 0;
}
#endif
