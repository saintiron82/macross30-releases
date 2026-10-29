#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "ppu_wait_backoff.h"
#include <atomic>
#include <mutex>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include "ps3emu/ps3_ftrace.h"
extern "C" unsigned char* vm_base;
extern "C" volatile uint32_t g_spu_put_wake_ea;   /* runtime/spu/spu_dma.h */
#pragma comment(lib, "synchronization.lib")
namespace {
std::once_flag init_once;
std::atomic<bool> ready{false};
bool enabled=false,trace=false;
std::atomic<unsigned> trace_count{0};
std::atomic<unsigned long long> sleeps{0};
void configure(){const char*e=std::getenv("MACROSS_PPU_WAIT_BACKOFF");enabled=e&&std::strcmp(e,"1")==0;e=std::getenv("MACROSS_PPU_WAIT_TRACE");trace=e&&std::strcmp(e,"1")==0;ready.store(true,std::memory_order_release);}
bool code_matches(uint32_t pc){
 static const unsigned char a[16]={0x88,0x03,0,0,0x2f,0x80,0,0,0x41,0x9e,0xff,0xf8,0x4e,0x80,0,0x20};
 static const unsigned char b[16]={0x88,0x1f,0,0,0x2f,0x80,0,0,0x41,0x9e,0xff,0xf8,0x88,0x1f,0x01,0x1b};
 const auto*expected=pc==0x001a279c?a:pc==0x0009b220?b:nullptr;
 return expected&&vm_base&&std::memcmp(vm_base+pc,expected,16)==0;
}
}
extern "C" int macross_ppu_wait_backoff_enabled(void){
 if(!ready.load(std::memory_order_acquire))std::call_once(init_once,configure);
 return enabled||trace;
}
extern "C" void macross_ppu_wait_backoff(uint32_t pc,uint32_t ea,uint32_t n){
 /* PS3_FTRACE timeline: kind 30 = this thread started waiting on the flag at
  * ea (first failed read), kind 31 = still waiting 4096 reads later. */
 if(ps3_ft_on()){const uint64_t t=ps3_ft_now();ps3_ft_span(n==1?30:31,(uint32_t)GetCurrentThreadId(),ea,pc,t,t);}
 if(trace&&n==1){unsigned ticket=trace_count.fetch_add(1,std::memory_order_relaxed);if(ticket<8)std::fprintf(stderr,"[ppu-wait] invocation=%u pc=%08X wait_ea=%08X code_guard=%d backoff=%d every=4096\n",ticket+1,pc,ea,code_matches(pc),enabled);}
 if(enabled&&(n&4095u)==0&&code_matches(pc)){
  sleeps.fetch_add(1,std::memory_order_relaxed);
  /* The byte is set by an SPU job's PUT (animation, image 10006). Park on it
   * with WaitOnAddress -- the PUT path wakes this address -- instead of a
   * timer Sleep(1) that always overshoots to >= 1 ms; 1 ms stays the bound for
   * any other writer. One parked thread at a time; others sleep as before. */
  if(InterlockedCompareExchange((volatile LONG*)&g_spu_put_wake_ea,(LONG)ea,0)==0){
   /* A wait-satisfied wake carries a dynamic priority boost; on a busy
    * machine the boosted guest thread then preempts the renderer (measured
    * -1.2 FPS at the renderer-bound save point). No boost for this thread. */
   static thread_local bool unboosted;
   if(!unboosted){unboosted=true;SetThreadPriorityBoost(GetCurrentThread(),TRUE);}
   unsigned char zero=0;
   WaitOnAddress((volatile void*)(vm_base+ea),&zero,1,1);
   InterlockedExchange((volatile LONG*)&g_spu_put_wake_ea,0);
  }else Sleep(1);
 }
}
