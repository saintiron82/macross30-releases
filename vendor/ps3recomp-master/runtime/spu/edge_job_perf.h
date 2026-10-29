/* Sampled host-only geometry phase timings. SPU body includes private PUT
 * recording; nested phase totals across workers overlap and are not FPS. */
#ifndef EDGE_JOB_PERF_H
#define EDGE_JOB_PERF_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
enum { EJP_NATIVE, EJP_SETUP, EJP_BODY, EJP_POST, EJP_VERIFY, EJP_CLEANUP, EJP_PHASES };
typedef struct { uint64_t last,start,phase[EJP_PHASES]; unsigned active,current,index; } ejp_scope;
typedef struct { uint64_t candidates,samples,failures,total,phase[EJP_PHASES],last,reports; } ejp_bucket;
static SPU_THREAD_LOCAL ejp_bucket ejp_buckets[12];
static INIT_ONCE ejp_once=INIT_ONCE_STATIC_INIT;
static atomic_int ejp_ready;
static int ejp_enabled;
static uint64_t ejp_frequency;
static BOOL CALLBACK ejp_init(PINIT_ONCE a,PVOID b,PVOID* c) {
    (void)a;(void)b;(void)c;
    const char* e=getenv("EDGE_JOB_PERF");
    LARGE_INTEGER f={0};
    if(e && strcmp(e,"1")==0 && QueryPerformanceFrequency(&f) && f.QuadPart>0) {
        ejp_enabled=1;ejp_frequency=(uint64_t)f.QuadPart;
    }
    atomic_store_explicit(&ejp_ready,1,memory_order_release);return TRUE;
}
static uint64_t ejp_clock(void) { LARGE_INTEGER q={0};QueryPerformanceCounter(&q);return (uint64_t)q.QuadPart; }
static ejp_scope ejp_begin(int image) {
    ejp_scope s={0};
    if(!((image>=10003 && image<=10005)||(image>=10011 && image<=10014)))return s;
    if(!atomic_load_explicit(&ejp_ready,memory_order_acquire))InitOnceExecuteOnce(&ejp_once,ejp_init,NULL,NULL);
    if(!ejp_enabled)return s;
    s.index=(unsigned)(image-10003);ejp_bucket* b=&ejp_buckets[s.index];
    if(b->candidates++ % 64u)return s;
    s.active=1;s.last=s.start=ejp_clock();
    if(!b->last)b->last=s.start;
    return s;
}
static void ejp_next(ejp_scope* s,unsigned phase) {
    if(!s->active)return;
    const uint64_t now=ejp_clock();
    if(now>=s->last)s->phase[s->current]+=now-s->last;
    s->last=now;s->current=phase;
}
static void ejp_end(ejp_scope* s,int failed) {
    if(!s->active)return;
    ejp_next(s,s->current);s->active=0;
    ejp_bucket* b=&ejp_buckets[s->index];b->samples++;b->failures+=failed!=0;
    if(s->last>=s->start)b->total+=s->last-s->start;
    for(unsigned i=0;i<EJP_PHASES;i++)b->phase[i]+=s->phase[i];
    if(s->last<b->last || s->last-b->last<ejp_frequency*5u)return;
    b->last=s->last;b->reports++;
    uint64_t sum=0;for(unsigned i=0;i<EJP_PHASES;i++)sum+=b->phase[i];
    fprintf(stderr,"[edge-job-perf] thread=%lu image=%u every=64 samples=%llu failures=%llu total_ms=%.6f native_candidate_ms=%.6f setup_ms=%.6f spu_body_with_put_ms=%.6f postprocess_ms=%.6f verification_ms=%.6f cleanup_ms=%.6f phase_sum_ticks=%llu total_ticks=%llu qpc=%llu frequency=%llu report=%llu scope=thread_cumulative_sampled_wall body_includes_put=1\n",
        (unsigned long)GetCurrentThreadId(),s->index+10003u,(unsigned long long)b->samples,(unsigned long long)b->failures,
        (double)b->total*1000.0/ejp_frequency,(double)b->phase[0]*1000.0/ejp_frequency,
        (double)b->phase[1]*1000.0/ejp_frequency,(double)b->phase[2]*1000.0/ejp_frequency,
        (double)b->phase[3]*1000.0/ejp_frequency,(double)b->phase[4]*1000.0/ejp_frequency,
        (double)b->phase[5]*1000.0/ejp_frequency,(unsigned long long)sum,(unsigned long long)b->total,
        (unsigned long long)s->last,(unsigned long long)ejp_frequency,(unsigned long long)b->reports);
}
#endif
