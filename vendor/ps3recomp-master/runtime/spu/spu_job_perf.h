/* Header-only host job diagnostics. Include in the one job dispatcher TU.
 * Full (image,size) keys, TLS staging and locked process-wide aggregation.
 * Cumulative wall time may overlap for nested jobs; it is not CPU time. */
#ifndef PS3RECOMP_SPU_JOB_PERF_H
#define PS3RECOMP_SPU_JOB_PERF_H
#include <windows.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define SPU_JP_KEYS 64u
#define SPU_JP_REPORTS 64u
typedef struct {
    uint64_t wall_start,cpu_start;
    unsigned index,active,cpu_valid;
    uint32_t desc;
} spu_job_perf_scope;
typedef struct {
    int image,last_result;uint32_t size,last_desc;
    uint64_t calls,success,failure,cpu_fail,wall,cpu,max_wall,max_cpu,last_completed;
} spu_jp_bucket;
typedef struct {
    spu_jp_bucket keys[SPU_JP_KEYS],overflow;
    unsigned count;uint64_t last_report;
} spu_jp_thread;
static __declspec(thread) spu_jp_thread spu_jp_tls;
static spu_jp_thread spu_jp_global;
static SRWLOCK spu_jp_lock=SRWLOCK_INIT;
static INIT_ONCE spu_jp_once=INIT_ONCE_STATIC_INIT;
static atomic_int spu_jp_ready;
static int spu_jp_enabled;
static unsigned spu_jp_interval=5,spu_jp_report_limit=SPU_JP_REPORTS;
static uint64_t spu_jp_frequency;
static volatile LONG spu_jp_reports;
static BOOL CALLBACK spu_jp_init(PINIT_ONCE a,PVOID b,PVOID* c) {
    (void)a;(void)b;(void)c;
    const char* e=getenv("SPU_JOBPROF");
    spu_jp_enabled=e && *e && strcmp(e,"0")!=0;
    if(spu_jp_enabled) {
        const char* value=getenv("SPU_JOBPROF_REPORTS");
        if(value && *value) {
            char* end=NULL;unsigned long count=strtoul(value,&end,10);
            if(end!=value && !*end && count<=1000000ul)spu_jp_report_limit=(unsigned)count;
        }
        char* end=NULL;long seconds=strtol(e,&end,10);
        if(end!=e && !*end)spu_jp_interval=seconds<1?1:seconds>3600?3600:(unsigned)seconds;
        LARGE_INTEGER f={0};
        if(QueryPerformanceFrequency(&f) && f.QuadPart>0)spu_jp_frequency=(uint64_t)f.QuadPart;
        else spu_jp_enabled=0;
        if(spu_jp_enabled){LARGE_INTEGER q={0};QueryPerformanceCounter(&q);spu_jp_global.last_report=(uint64_t)q.QuadPart;}
    }
    atomic_store_explicit(&spu_jp_ready,1,memory_order_release);return TRUE;
}
static uint64_t spu_jp_wall(void) {
    LARGE_INTEGER q={0};QueryPerformanceCounter(&q);return (uint64_t)q.QuadPart;
}
static int spu_jp_cpu(uint64_t* value) {
    FILETIME c,e,k,u;
    if(!GetThreadTimes(GetCurrentThread(),&c,&e,&k,&u))return 0;
    *value=(((uint64_t)k.dwHighDateTime<<32)|k.dwLowDateTime)+
           (((uint64_t)u.dwHighDateTime<<32)|u.dwLowDateTime);return 1;
}
static void spu_jp_row(const spu_jp_bucket* b,int overflow) {
    fprintf(stderr,"[spu-jobprof] reporter_thread=%lu image=%d size=%u key=%s calls=%llu success=%llu failure=%llu last_desc=%08X last_result=%d wall_ms=%.6f cpu_ms=%.6f max_wall_ms=%.6f max_cpu_ms=%.6f cpu_fail=%llu totals=process_cumulative scope=inclusive_overlap_across_threads\n",
        (unsigned long)GetCurrentThreadId(),b->image,b->size,overflow?"overflow_unattributed":"full_image_size",
        (unsigned long long)b->calls,(unsigned long long)b->success,(unsigned long long)b->failure,
        b->last_desc,b->last_result,(double)b->wall*1000.0/spu_jp_frequency,(double)b->cpu/10000.0,
        (double)b->max_wall*1000.0/spu_jp_frequency,(double)b->max_cpu/10000.0,(unsigned long long)b->cpu_fail);
}
static void spu_jp_report(uint64_t now) {
    if(!spu_jp_global.last_report){spu_jp_global.last_report=now;return;}
    if(now<spu_jp_global.last_report || now-spu_jp_global.last_report<spu_jp_frequency*spu_jp_interval)return;
    spu_jp_global.last_report=now;
    const unsigned report=(unsigned)InterlockedIncrement(&spu_jp_reports);
    if(spu_jp_report_limit && report>spu_jp_report_limit)return;
    fprintf(stderr,"[spu-jobprof-window] reporter_thread=%lu interval_seconds=%u keys=%u cap=%u qpc=%llu frequency=%llu totals=process_cumulative scope=inclusive_overlap_across_threads\n",
        (unsigned long)GetCurrentThreadId(),spu_jp_interval,spu_jp_global.count,spu_jp_report_limit,
        (unsigned long long)now,(unsigned long long)spu_jp_frequency);
    for(unsigned i=0;i<spu_jp_global.count;i++)spu_jp_row(&spu_jp_global.keys[i],0);
    if(spu_jp_global.overflow.calls)spu_jp_row(&spu_jp_global.overflow,1);
}
static void spu_jp_merge(spu_jp_bucket* dst,spu_jp_bucket* src) {
    dst->calls+=src->calls;dst->success+=src->success;dst->failure+=src->failure;
    dst->cpu_fail+=src->cpu_fail;dst->wall+=src->wall;dst->cpu+=src->cpu;
    if(src->max_wall>dst->max_wall)dst->max_wall=src->max_wall;
    if(src->max_cpu>dst->max_cpu)dst->max_cpu=src->max_cpu;
    if(src->last_completed>=dst->last_completed) {
        dst->last_completed=src->last_completed;dst->last_desc=src->last_desc;dst->last_result=src->last_result;
    }
    /* Keep key identities stable: another nested scope can still hold their
     * indices. Only completed counters are consumed by this flush. */
    const int image=src->image;const uint32_t size=src->size;
    memset(src,0,sizeof *src);src->image=image;src->size=size;
}
static void spu_job_perf_flush(void) {
    if(!atomic_load_explicit(&spu_jp_ready,memory_order_acquire) || !spu_jp_enabled)return;
    const uint64_t now=spu_jp_wall();
    AcquireSRWLockExclusive(&spu_jp_lock);
    for(unsigned i=0;i<spu_jp_tls.count;i++) {
        spu_jp_bucket* src=&spu_jp_tls.keys[i];if(!src->calls)continue;
        unsigned j=0;
        while(j<spu_jp_global.count && (spu_jp_global.keys[j].image!=src->image || spu_jp_global.keys[j].size!=src->size))j++;
        if(j==spu_jp_global.count && j<SPU_JP_KEYS) {
            spu_jp_global.keys[j].image=src->image;spu_jp_global.keys[j].size=src->size;spu_jp_global.count++;
        }
        spu_jp_merge(j<SPU_JP_KEYS?&spu_jp_global.keys[j]:&spu_jp_global.overflow,src);
    }
    if(spu_jp_tls.overflow.calls)spu_jp_merge(&spu_jp_global.overflow,&spu_jp_tls.overflow);
    spu_jp_report(now);ReleaseSRWLockExclusive(&spu_jp_lock);
    spu_jp_tls.last_report=now;
}
static spu_job_perf_scope spu_job_perf_begin(int image,uint32_t size,uint32_t desc_ea) {
    spu_job_perf_scope s={0};
    if(!atomic_load_explicit(&spu_jp_ready,memory_order_acquire))
        InitOnceExecuteOnce(&spu_jp_once,spu_jp_init,NULL,NULL);
    if(!spu_jp_enabled)return s;
    unsigned index=0;
    while(index<spu_jp_tls.count && (spu_jp_tls.keys[index].image!=image || spu_jp_tls.keys[index].size!=size))index++;
    if(index==spu_jp_tls.count && index<SPU_JP_KEYS) {
        spu_jp_tls.keys[index].image=image;spu_jp_tls.keys[index].size=size;spu_jp_tls.count++;
    }
    s.index=index;s.active=1;s.desc=desc_ea;s.wall_start=spu_jp_wall();
    s.cpu_valid=(unsigned)spu_jp_cpu(&s.cpu_start);
    if(!spu_jp_tls.last_report)spu_jp_tls.last_report=s.wall_start;
    return s;
}
static void spu_job_perf_end(spu_job_perf_scope* s,int result) {
    if(!s || !s->active)return;s->active=0;
    uint64_t cpu=0;const uint64_t now=spu_jp_wall();const int cpu_ok=spu_jp_cpu(&cpu);
    spu_jp_bucket* b=s->index<SPU_JP_KEYS?&spu_jp_tls.keys[s->index]:&spu_jp_tls.overflow;
    b->calls++;b->last_desc=s->desc;b->last_result=result;
    b->last_completed=now;
    if(result==0)b->success++;else b->failure++;
    if(now>=s->wall_start){const uint64_t d=now-s->wall_start;b->wall+=d;if(d>b->max_wall)b->max_wall=d;}
    if(s->cpu_valid && cpu_ok && cpu>=s->cpu_start) {
        const uint64_t d=cpu-s->cpu_start;b->cpu+=d;if(d>b->max_cpu)b->max_cpu=d;
    } else b->cpu_fail++;
    if(now>=spu_jp_tls.last_report && now-spu_jp_tls.last_report>=spu_jp_frequency*spu_jp_interval)
        spu_job_perf_flush();
}
#endif
