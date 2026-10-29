/* Disjoint CPU-side wall intervals. These are not GPU timings and do not
 * become exclusive thread CPU time merely because they are disjoint. */
#ifndef PS3RECOMP_RSX_CPU_DRAW_PHASES_H
#define PS3RECOMP_RSX_CPU_DRAW_PHASES_H
#include "ps3emu/ps3types.h"
enum { LD_CPU_PHASE_NONE=-1, LD_CPU_PHASE_PSO=0, LD_CPU_PHASE_TEXTURE,
       LD_CPU_PHASE_DESCRIPTORS, LD_CPU_PHASE_CONSTANTS, LD_CPU_PHASE_STATE,
       LD_CPU_PHASE_DRAW, LD_CPU_PHASE_COUNT };
typedef struct rsx_cpu_draw_phase_scope {
    u32 depth;
    int enabled, active;
    u64 start;
} rsx_cpu_draw_phase_scope;
typedef u64 (*rsx_cpu_draw_phase_clock)(void);

static void rsx_cpu_draw_phase_begin(rsx_cpu_draw_phase_scope* s,int enabled)
{
    if(++s->depth==1u){s->enabled=enabled;s->active=LD_CPU_PHASE_NONE;s->start=0;}
}
static int rsx_cpu_draw_phase_switch(rsx_cpu_draw_phase_scope* s,int next,
    rsx_cpu_draw_phase_clock clock,u64 totals[LD_CPU_PHASE_COUNT],u64 samples[LD_CPU_PHASE_COUNT])
{
    if(s->depth!=1u || !s->enabled)return LD_CPU_PHASE_NONE;
    const int previous=s->active;
    if(previous==next)return previous;
    const u64 now=clock();
    if(previous>=0 && previous<LD_CPU_PHASE_COUNT){
        if(now>=s->start)totals[previous]+=now-s->start;
        samples[previous]++;
    }
    s->active=next;s->start=next==LD_CPU_PHASE_NONE?0:now;
    return previous;
}
static void rsx_cpu_draw_phase_end(rsx_cpu_draw_phase_scope* s,
    rsx_cpu_draw_phase_clock clock,u64 totals[LD_CPU_PHASE_COUNT],u64 samples[LD_CPU_PHASE_COUNT])
{
    if(!s->depth)return;
    if(s->depth==1u)rsx_cpu_draw_phase_switch(s,LD_CPU_PHASE_NONE,clock,totals,samples);
    s->depth--;
}
#endif
