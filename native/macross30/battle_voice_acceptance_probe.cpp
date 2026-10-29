// Diagnostic observer only: never changes guest memory, audio or caption admission.
#include "battle_voice_acceptance_probe.h"
#include "recompiled/ppu_recomp.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace {
constexpr uint32_t core_toc=0x00AC1BA0;
std::atomic<unsigned> events{0};
std::atomic<unsigned long long> generations{0};
struct Request { ppu_context* ctx{}; uint8_t* memory{}; unsigned long long generation{};
    uint32_t stream{}; int32_t before_state{}; std::array<char,320> path{}; };
thread_local Request pending;
bool enabled() { static const bool value=[] { const char* p=std::getenv("MACROSS_BATTLE_VOICE_ACCEPTANCE_DIAG"); return p&&std::strcmp(p,"1")==0; }();return value; }
bool readable(uint32_t ea, size_t bytes) {
    if(!vm_base||!ea||uint64_t(ea)+bytes>0x100000000ULL) return false;
    uintptr_t cursor=reinterpret_cast<uintptr_t>(vm_base)+ea, end=cursor+bytes;
    if(cursor<reinterpret_cast<uintptr_t>(vm_base)||end<cursor) return false;
    while(cursor<end) { MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery(reinterpret_cast<void*>(cursor),&info,sizeof(info))||info.State!=MEM_COMMIT||
           (info.Protect&(PAGE_GUARD|PAGE_NOACCESS))) return false;
        const DWORD p=info.Protect&255;
        if(p!=PAGE_READONLY&&p!=PAGE_READWRITE&&p!=PAGE_WRITECOPY&&p!=PAGE_EXECUTE_READ&&
           p!=PAGE_EXECUTE_READWRITE&&p!=PAGE_EXECUTE_WRITECOPY) return false;
        uintptr_t next=reinterpret_cast<uintptr_t>(info.BaseAddress)+info.RegionSize;
        if(next<=cursor) return false;cursor=next;
    }return true;
}
bool text(uint32_t ea,std::array<char,320>& out) {
    for(size_t i=0;i<out.size();++i) { if(uint64_t(ea)+i>0xFFFFFFFFULL||!readable(ea+uint32_t(i),1))return false;
        unsigned char c=vm_read8(ea+uint32_t(i)); if(!c){out[i]=0;return true;}
        // Paths are ASCII. Refuse control bytes, untrusted log escape/newline injection.
        if(c<32||c>126)return false;out[i]=char(c);
    }return false;
}
uint32_t stream() { return readable(core_toc+0x7D4,4)?vm_read32(core_toc+0x7D4):0; }
}
extern "C" void macross_battle_voice_probe_request(ppu_context* ctx,uint32_t ea) {
    if(!enabled())return;
    pending={};if(!ctx)return;
    Request value{};value.ctx=ctx;value.memory=vm_base;value.generation=++generations;
    value.stream=stream();if(!readable(value.stream,8)||!text(ea,value.path))return;
    value.before_state=int32_t(vm_read32(value.stream));pending=value;
}
extern "C" void macross_battle_voice_probe_open(ppu_context* ctx,uint32_t channel,int32_t result) {
    if(!enabled())return;
    Request value=pending;pending={};
    if(!ctx||value.ctx!=ctx||value.memory!=vm_base||!channel||channel!=value.stream||channel!=stream())return;
    if(events.fetch_add(1)>=64)return;
    // At LR1C4C18 the veneer has restored the caller's core TOC. The audio
    // veneer used (coreTOC+0x10000-1252); its +75C4 points to the handle table.
    constexpr uint32_t audio_toc=core_toc+0x10000-1252;
    uint32_t lower=0,table=0;int32_t handle=-1,state=-1;uint32_t error=0xFFFFFFFF;
    // 1C4BB0 saves incoming physical container/offset in nonvolatile r27/r28.
    // Offset is a 32-bit byte position, even when the guest register is sign extended.
    const uint32_t expected_offset=uint32_t(ctx->gpr[28]);
    uint32_t actual_offset=0;bool lower_valid=false;
    bool path_valid=false;std::array<char,320> actual{},expected_container{};
    const bool expected_valid=text(uint32_t(ctx->gpr[27]),expected_container);
    if(readable(channel,8))handle=int32_t(vm_read32(channel+4));
    if(readable(audio_toc+0x75C4,4))table=vm_read32(audio_toc+0x75C4);
    if(handle>=0&&readable(table,4)&&uint32_t(handle)<vm_read32(table)&&
       uint64_t(table)+4+uint64_t(uint32_t(handle))*4<=0xFFFFFFFCULL) {
        uint32_t slot=table+4+uint32_t(handle)*4;
        if(readable(slot,4))lower=vm_read32(slot);
    }
    if(readable(lower,0x3E0)) { lower_valid=true;actual_offset=vm_read32(lower+0x144);state=int32_t(vm_read32(lower+0x274));error=vm_read32(lower+0x3DC);path_valid=text(lower+0x40,actual); }
    if(value.memory!=vm_base)return;
    std::fprintf(stderr,"[battle-voice-acceptance] generation=%llu thread=%lu stream=%08X prior_stream_state=%d result=%08X handle=%d lower=%08X lower_state=%d error=%08X request=\"%s\" expected_container=\"%s\" expected_offset=%08X resolved=\"%s\" actual_offset=%08X offset_readable=%s container_offset_match=%s scheduling_only=yes\n",
        value.generation,static_cast<unsigned long>(GetCurrentThreadId()),channel,value.before_state,uint32_t(result),handle,lower,state,error,
        value.path.data(),expected_valid?expected_container.data():"<unreadable>",expected_offset,
        path_valid?actual.data():"<unreadable>",actual_offset,lower_valid?"yes":"no",
        expected_valid&&path_valid&&lower_valid&&expected_offset==actual_offset&&
        std::strcmp(expected_container.data(),actual.data())==0?"yes":"no");
}
