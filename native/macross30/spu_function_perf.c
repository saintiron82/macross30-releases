/* Host diagnostics only. No architectural state writes, stacks, wakeups or
 * completion synthesis. Unwound/longjmp scopes remain started-not-completed. */
#include "spu_function_perf.h"
#include "spu/spu_context.h"
#include <windows.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define SPF_BUCKETS 512u
#define SPF_TOP 16u
typedef struct {int image;uint32_t pc;uint64_t entries,started,completed,wall,cpu,cpu_fail;} spf_bucket;
typedef struct {spf_bucket bucket[SPF_BUCKETS];unsigned count;uint64_t overflow,last_report;} spf_thread;
static __declspec(thread) spf_thread spf;
static INIT_ONCE spf_once=INIT_ONCE_STATIC_INIT;
static atomic_int spf_ready;
static int spf_enabled;
static unsigned spf_interval=64,spf_report_cap=32;
static uint64_t spf_frequency;
static volatile LONG spf_reports;
static unsigned spf_env_range(const char* name,unsigned fallback,unsigned maximum) {
    const char* s=getenv(name);if(!s || !*s)return fallback;
    char* end=NULL;unsigned long n=strtoul(s,&end,10);
    return end!=s && !*end && n>=1 && n<=maximum?(unsigned)n:fallback;
}
static BOOL CALLBACK spf_init(PINIT_ONCE a,PVOID b,PVOID* c) {
    (void)a;(void)b;(void)c;
    const char* e=getenv("MACROSS_SPU_FUNCTION_PERF");spf_enabled=e && atoi(e)!=0;
    spf_interval=spf_env_range("MACROSS_SPU_FUNCTION_PERF_INTERVAL",64,4096);
    spf_report_cap=spf_env_range("MACROSS_SPU_FUNCTION_PERF_REPORTS",32,128);
    LARGE_INTEGER f={0};if(spf_enabled && QueryPerformanceFrequency(&f) && f.QuadPart>0)
        spf_frequency=(uint64_t)f.QuadPart;
    else spf_enabled=0;
    atomic_store_explicit(&spf_ready,1,memory_order_release);return TRUE;
}
static uint64_t spf_wall(void){LARGE_INTEGER q={0};QueryPerformanceCounter(&q);return (uint64_t)q.QuadPart;}
static int spf_cpu(uint64_t* value) {
    FILETIME c,e,k,u;
    if(!GetThreadTimes(GetCurrentThread(),&c,&e,&k,&u))return 0;
    *value=(((uint64_t)k.dwHighDateTime<<32)|k.dwLowDateTime)+
           (((uint64_t)u.dwHighDateTime<<32)|u.dwLowDateTime);return 1;
}
static void spf_report(uint64_t now) {
    if(now<spf.last_report || now-spf.last_report<spf_frequency*5u)return;
    spf.last_report=now;
    if((unsigned)InterlockedIncrement(&spf_reports)>spf_report_cap)return;
    unsigned selected[SPF_TOP],n=0;
    for(unsigned i=0;i<spf.count;i++) {
        unsigned pos=0;
        while(pos<n && (spf.bucket[selected[pos]].cpu>spf.bucket[i].cpu ||
              (spf.bucket[selected[pos]].cpu==spf.bucket[i].cpu &&
               spf.bucket[selected[pos]].wall>=spf.bucket[i].wall)))pos++;
        if(pos>=SPF_TOP)continue;
        if(n<SPF_TOP)n++;
        for(unsigned j=n-1;j>pos;j--)selected[j]=selected[j-1];selected[pos]=i;
    }
    fprintf(stderr,"[spu-function-perf] thread=%lu interval=%u buckets=%u overflow_entries=%llu scope=inclusive_overlap totals=cumulative sampling=per_image_pc_first_then_interval incomplete=started_minus_completed_including_active_or_longjmp\n",
        (unsigned long)GetCurrentThreadId(),spf_interval,spf.count,(unsigned long long)spf.overflow);
    for(unsigned j=0;j<n;j++) {
        const spf_bucket* b=&spf.bucket[selected[j]];
        fprintf(stderr,"[spu-function-perf-row] thread=%lu image=%d pc=%05X entries=%llu sampled_started=%llu sampled_completed=%llu incomplete=%llu sampled_wall_ms=%.6f sampled_cpu_ms=%.6f cpu_fail=%llu\n",
            (unsigned long)GetCurrentThreadId(),b->image,b->pc,(unsigned long long)b->entries,
            (unsigned long long)b->started,(unsigned long long)b->completed,
            (unsigned long long)(b->started-b->completed),(double)b->wall*1000.0/spf_frequency,
            (double)b->cpu/10000.0,(unsigned long long)b->cpu_fail);
    }
}
macross_spu_function_scope macross_spu_function_begin(const struct spu_context* ctx,uint32_t pc) {
    macross_spu_function_scope scope={0};
    if(!atomic_load_explicit(&spf_ready,memory_order_acquire))InitOnceExecuteOnce(&spf_once,spf_init,NULL,NULL);
    if(!spf_enabled || !ctx)return scope;
    unsigned index=0;
    while(index<spf.count && (spf.bucket[index].image!=ctx->image_id || spf.bucket[index].pc!=pc))index++;
    if(index==spf.count) {
        if(index>=SPF_BUCKETS){spf.overflow++;return scope;}
        spf.bucket[index].image=ctx->image_id;spf.bucket[index].pc=pc;spf.count++;
    }
    spf_bucket* b=&spf.bucket[index];b->entries++;
    if((b->entries-1u)%spf_interval)return scope;
    scope.bucket=index;scope.sampled=1;scope.wall_start=spf_wall();
    scope.cpu_valid=(unsigned)spf_cpu(&scope.cpu_start);b->started++;
    if(!spf.last_report)spf.last_report=scope.wall_start;
    return scope;
}
void macross_spu_function_end(macross_spu_function_scope* scope) {
    if(!scope || !scope->sampled)return;
    scope->sampled=0; /* a repeated cleanup cannot double-account */
    const uint64_t now=spf_wall();uint64_t cpu=0;
    const int cpu_ok=spf_cpu(&cpu);
    spf_bucket* b=&spf.bucket[scope->bucket];b->completed++;
    if(now>=scope->wall_start)b->wall+=now-scope->wall_start;
    if(scope->cpu_valid && cpu_ok && cpu>=scope->cpu_start)b->cpu+=cpu-scope->cpu_start;
    else b->cpu_fail++;
    spf_report(now);
}
