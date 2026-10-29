/*
 * ps3recomp game project -- game-specific hooks and overrides.
 *
 * main.cpp is the runner and is game-agnostic. This file is where a port puts
 * everything that is not: the PRX loading hook it must answer, overrides for
 * firmware functions the HLE library gets wrong for this title, and patches
 * over recompiled code.
 *
 * It starts out as the empty version of all of that, which is what a game with
 * no lifted PRX and no overrides needs.
 */

#include <cstdio>
#include <cstring>

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

/* ---------------------------------------------------------------------------
 * PRX loading
 *
 * main.cpp calls this once, after the lifted function table is registered and
 * vm_base is live, before the game runs. A title that lifts a real system PRX
 * -- libsre, which is cellSpurs and cellSync -- loads its image into guest RAM
 * here and registers its exports. Most ports never need to: the HLE library
 * answers those modules instead.
 * -----------------------------------------------------------------------*/

extern "C" void ps3_load_prx_modules(void)
{
}

/* ---------------------------------------------------------------------------
 * Example: Override a specific NID
 *
 * The module system resolves function imports by NID.  To replace a default
 * HLE stub with custom logic, register your function at init time.
 * -----------------------------------------------------------------------*/

/*
 * Example: override cellGcmSetFlipMode (NID 0xA53D12AE) to force vsync.
 *
 *   static void my_cellGcmSetFlipMode(u32 mode)
 *   {
 *       printf("[hook] cellGcmSetFlipMode: forcing VSYNC (was %u)\n", mode);
 *       // Force vsync regardless of what the game requested
 *       cellGcmSetFlipMode(CELL_GCM_DISPLAY_VSYNC);
 *   }
 *
 * Then in your init code:
 *
 *   ps3::modules::override_nid(0xA53D12AE,
 *       reinterpret_cast<void*>(my_cellGcmSetFlipMode));
 */

/* ---------------------------------------------------------------------------
 * Example: Hook into module loading
 *
 * You can intercept cellSysmoduleLoadModule to perform custom actions
 * when a particular module is loaded.
 * -----------------------------------------------------------------------*/

/*
 *   static s32 my_loadmodule_hook(u16 id)
 *   {
 *       printf("[hook] Module 0x%04X loaded\n", id);
 *
 *       if (id == CELL_SYSMODULE_AUDIO) {
 *           // Perform custom audio backend initialization
 *           init_my_audio_backend();
 *       }
 *
 *       // Call the original implementation
 *       return cellSysmoduleLoadModule(id);
 *   }
 */

/* ---------------------------------------------------------------------------
 * Game-specific patches
 *
 * Sometimes you need to patch recompiled code at specific addresses.
 * Use the patch system to modify behavior without editing generated code.
 * -----------------------------------------------------------------------*/

/*
 *   // Skip an integrity check at guest address 0x00812FA0
 *   ps3::patches::nop_range(0x00812FA0, 0x00812FB0);
 *
 *   // Replace a function call with a direct return
 *   ps3::patches::force_return(0x009A1000, 0);  // return 0
 */

/* Guest LFQueue bring-up bridge. Enabled explicitly until the real lifted
 * consumer has verified publication, dequeue, payload and completion. */
#include "ppu_context.h"
#include "native_zlib.h"
#include <cstdlib>
#include <mutex>
#include <thread>
#include <chrono>
extern "C" {
void cellGcm_debug_snapshot(void);
extern uint8_t* vm_base;
void ps3_hle_register_ctx(uint32_t, const char*, void (*)(ppu_context*));
void spu_lockline_lock(void);
void spu_lockline_unlock(void);
void spu_coh_notify_write(uint32_t);
int spu_taskset_signal_parked_obj(uint32_t, uint32_t);
void spu_taskset_latch_wake(uint32_t);
uint32_t spurs_registered_tasksets(uint32_t*,uint32_t);
void spu_taskset_signal_task(uint32_t,uint32_t);
int32_t cellSpursEventFlagWait(void*,void*,uint32_t);
ppu_context* ppu_debug_thread_named(const char*);
}
static uint32_t lf_u32(uint32_t p) {
    return (uint32_t(vm_base[p])<<24)|(uint32_t(vm_base[p+1])<<16)|
           (uint32_t(vm_base[p+2])<<8)|vm_base[p+3];
}
static uint16_t lf_u16(uint32_t p) { return (vm_base[p]<<8)|vm_base[p+1]; }
static void lf_w16(uint32_t p,uint16_t v) { vm_base[p]=v>>8; vm_base[p+1]=v; }
static void lf_w32(uint32_t p,uint32_t v) {
    vm_base[p]=v>>24; vm_base[p+1]=v>>16; vm_base[p+2]=v>>8; vm_base[p+3]=v;
}
static std::mutex lf_producer_lock;
static uint32_t lf_last_queue;
struct LfProbe { uint32_t src,dst,input,output; };
static LfProbe lf_probes[8];
static unsigned lf_probe_count;
static unsigned lf_probe_batch;
static void lf_save_probe(unsigned i,const LfProbe& p,bool output) {
    uint32_t addr=output?p.dst:p.src,n=output?p.output:p.input;
    if (!addr||!n||n>65536||uint64_t(addr)+n>0x100000000ull) return;
    char path[96];
    snprintf(path,sizeof path,"lf_batch_%u_probe_%u_%s.bin",lf_probe_batch,i,output?"output":"input");
    if (FILE* f=fopen(path,"wb")) { fwrite(vm_base+addr,1,n,f); fclose(f); }
}
static void macross_lf_completion(ppu_context* c) {
    const int32_t rc=cellSpursEventFlagWait((void*)(uintptr_t)c->gpr[3],
                                         (void*)(uintptr_t)c->gpr[4],uint32_t(c->gpr[5]));
    // The probe covers only the first four data batches. Later unrelated
    // event waits must never block on the queue producer's diagnostic mutex.
    if (!rc) macross_native_zlib_after_wait(uint32_t(c->gpr[3]));
    if (!rc && lf_probe_batch<4) {
        std::lock_guard<std::mutex> guard(lf_producer_lock);
        if (lf_probe_batch<4) for(unsigned i=0;i<lf_probe_count;++i) lf_save_probe(i,lf_probes[i],true);
        lf_probe_count=0; ++lf_probe_batch;
    }
    c->gpr[3]=uint64_t(int64_t(rc));
}
static void macross_lf_init(ppu_context* c) {
    uint32_t owner=c->gpr[3],q=c->gpr[4],buf=c->gpr[5];
    uint32_t size=c->gpr[6],depth=c->gpr[7],dir=c->gpr[8];
    if (!q||!buf||!size||!depth||depth>0x7fff||dir!=3||q%128||size%16) {
        c->gpr[3]=uint64_t(int64_t(int32_t(0x80410902))); return;
    }
    // CellSyncLFQueue ABI: halfword cursors; ANY2ANY sentinel slots.
    memset(vm_base+q,0,128);
    lf_w32(q+0x10,size); lf_w32(q+0x14,depth); lf_w32(q+0x1c,buf|1);
    vm_base[q+0x20]=vm_base[q+0x21]=0xff;
    lf_w32(q+0x24,dir); lf_w32(q+0x28,0xffffffff);
    lf_w16(q+0x30,0xffff); lf_w16(q+0x50,0xffff);
    lf_w32(q+0x74,owner);
    fprintf(stderr,"[macross-lfq] init q=%08X owner=%08X buf=%08X size=%u depth=%u\n",q,owner,buf,size,depth);
    c->gpr[3]=0;
}
template<class F> static void lf_for_each_consumer(uint32_t q,F&& fn) {
    const uint32_t tagged_owner=lf_u32(q+0x74);
    const uint32_t owner=tagged_owner&~1u;
    uint32_t tasksets[64];
    const unsigned count=spurs_registered_tasksets(tasksets,64);
    for (unsigned s=0;s<count;++s) {
        const uint32_t ts=tasksets[s];
        if ((tagged_owner&1) ? lf_u32(ts+0x64)!=owner : ts!=owner) continue;
        for (unsigned t=0;t<128;++t) {
            const uint32_t task_args=ts+0x80+48*t;
            // This game's queue consumers receive the queue EA in arg word0.
            // Retain taskset identity even after other tasksets are created.
            if ((vm_base[ts+0x30+t/8] & (0x80u>>(t%8))) &&
                (lf_u32(task_args)&~7u)==q) fn(ts,t);
        }
    }
}
// Caller holds lf_producer_lock. Returns 0, or CELL_SYNC_ERROR_AGAIN when the
// queue is full and the push is non-blocking.
static int32_t lf_publish_locked(uint32_t q,const uint8_t* element,bool blocking) {
    const uint32_t size=lf_u32(q+0x10),depth=lf_u32(q+0x14);
    const uint32_t buf=lf_u32(q+0x1c)&~1u;
    uint16_t push;
    const char* window_env=getenv("MACROSS_LFQUEUE_WINDOW");
    unsigned window=window_env?unsigned(atoi(window_env)):15;
    if (!window||window>15) window=15;
    for (;;) {
        spu_lockline_lock();
        push=lf_u16(q+8);
        const uint16_t reserved=lf_u16(q+14),pop=lf_u16(q);
        const unsigned used=(push+2*depth-pop)%(2*depth);
    // Synchronous publication supports no overlapping guest producer reservation.
        // The lifted ANY2ANY consumer tracks in-flight completion with a
        // 16-bit mask (A410). Do not publish beyond that reorder window.
        if (used<depth && used<window && push==reserved) break;
        spu_lockline_unlock();
        if (!blocking) return int32_t(0x80410901);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    memcpy(vm_base+buf+(push%depth)*size,element,size);
    const uint16_t next=(push+1)%(2*depth);
    lf_w16(q+8,next); lf_w16(q+14,next);
    spu_coh_notify_write(q);
    spu_lockline_unlock();
    int woke=0;
    lf_for_each_consumer(q,[&](uint32_t ts,unsigned t){ spu_taskset_signal_task(ts,t); ++woke; });
    auto w=[element](unsigned i){ const uint8_t* p=element+4*i; return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3]; };
    static unsigned n;
    if (++n<=24) fprintf(stderr,"[macross-lfq] push#%u q=%08X slot=%u size=%u wake=%d payload=%08X %08X %08X %08X\n",n,q,push%depth,size,woke,w(0),w(1),w(2),w(3));
    return 0;
}
extern "C" int macross_lf_publish(uint32_t q,const uint8_t* element) {
    std::lock_guard<std::mutex> guard(lf_producer_lock);
    return lf_publish_locked(q,element,true);
}
// Is q consumed by the Edge zlib inflate task (SPU image 433707155)? The
// consumer's ELF (task info +0x14) carries the SPUNAME note at 0x8EB4.
// Caller holds lf_producer_lock.
static bool lf_zlib_queue(uint32_t q) {
    static uint32_t known[8]; static bool zlib[8]; static unsigned known_count;
    for (unsigned i=0;i<known_count;++i) if (known[i]==q) return zlib[i];
    static const char name[]="PS3_Release/edgezlib_inflate_tas";
    bool found=false,match=false;
    lf_for_each_consumer(q,[&](uint32_t ts,unsigned t){
        found=true;
        const uint32_t elf=lf_u32(ts+0x80+48*t+0x14)&~0xFu;
        if (elf && uint64_t(elf)+0x8EB4+32<=0x100000000ull &&
            !memcmp(vm_base+elf+0x8EB4,name,32)) match=true;
    });
    if (!found) return false;   // tasks not created yet: SPU path, ask again
    if (known_count<8) { known[known_count]=q; zlib[known_count++]=match; }
    fprintf(stderr,"[native-zlib] queue %08X consumer=%s\n",q,match?"edgezlib_inflate_task":"other");
    return match;
}
static void macross_lf_push(ppu_context* c) {
    const uint32_t q=c->gpr[3],src=c->gpr[4];
    std::lock_guard<std::mutex> guard(lf_producer_lock);
    if (!q||!src||q%128||lf_u32(q+0x24)!=3) {
        c->gpr[3]=uint64_t(int64_t(int32_t(0x80410902))); return;
    }
    const uint32_t size=lf_u32(q+0x10),depth=lf_u32(q+0x14);
    lf_last_queue=q;
    if (lf_probe_batch<4 && lf_probe_count<8) {
        auto& p=lf_probes[lf_probe_count];
        p={lf_u32(src),lf_u32(src+4),lf_u32(src+8),lf_u32(src+12)};
        lf_save_probe(lf_probe_count++,p,false);
    }
    if (!size||!depth||depth>0x7fff||size>0x4000) {
        c->gpr[3]=uint64_t(int64_t(int32_t(0x80410902))); return;
    }
    // Native mode decodes on host workers and never publishes, so the SPU
    // consumers stay parked in WAIT_SIGNAL; failures come back through
    // macross_lf_publish. Verify mode records a shadow and publishes as before.
    if (size==32 && macross_native_zlib_mode()!=MACROSS_ZLIB_OFF && lf_zlib_queue(q) &&
        macross_native_zlib_submit(q,vm_base+src)) {
        c->gpr[3]=0; return;
    }
    c->gpr[3]=uint64_t(int64_t(lf_publish_locked(q,vm_base+src,c->gpr[5]!=0)));
}
void macross_register_bridges(void) {
    if (!getenv("MACROSS_LFQUEUE_BRIDGE")) return;
    ps3_hle_register_ctx(0x011EE38B,"macross LFQueue initialize",macross_lf_init);
    ps3_hle_register_ctx(0x8A85674D,"macross LFQueue push",macross_lf_push);
    ps3_hle_register_ctx(0x373523D4,"macross LFQueue completion probe",macross_lf_completion);
    macross_native_zlib_mode();   // log the mode; reads no_native_zlib from the run directory
}
void macross_report_bridges(void) {
    cellGcm_debug_snapshot();
    if (!lf_last_queue) return;
    // Watchdog diagnostics are best effort. Never wait on a producer which
    // may itself be stalled; that would prevent the watchdog from terminating.
    uint32_t q=lf_last_queue;
    fprintf(stderr,"[macross-lfq] cursors pop=%u/%u/%u/%u push=%u/%u/%u/%u slots=%04X/%04X\n",
            lf_u16(q),lf_u16(q+2),lf_u16(q+4),lf_u16(q+6),
            lf_u16(q+8),lf_u16(q+10),lf_u16(q+12),lf_u16(q+14),lf_u16(q+0x30),lf_u16(q+0x50));
    auto* init=ppu_debug_thread_named("InitApp");
    if (init && uint32_t(init->lr)==0x0062D774) {
        uint32_t manager=init->gpr[31],id=init->gpr[28];
        if (manager>=0x10000000 && manager<0xD0000000) {
            uint32_t count=lf_u32(manager+0x10),entries=lf_u32(manager+0x20);
            fprintf(stderr,"[macross-filewait] manager=%08X id=%08X count=%u entries=%08X\n",manager,id,count,entries);
            if (count>0 && count<=256 && entries>=0x10000000 && uint64_t(entries)+count*0x4a0<0xD0000000) {
                if (FILE* f=fopen("pending_file_entries.bin","wb")) {
                    fwrite(vm_base+entries,0x4a0,count,f); fclose(f);
                }
            }
        }
    }
}
