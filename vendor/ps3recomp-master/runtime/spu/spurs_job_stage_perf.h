/* Opt-in sampled host phase wall timing. No guest state or CPU clock changes.
 * allocation includes descriptor validation; snapshot includes post-run diagnostics.
 * context includes pre-entry diagnostics. Sample overhead is diagnostic only. */
#ifndef SPURS_JOB_STAGE_PERF_H
#define SPURS_JOB_STAGE_PERF_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
enum { SJ_ALLOC, SJ_STAGE, SJ_CONTEXT, SJ_BODY, SJ_SNAPSHOT, SJ_CLEANUP, SJ_PHASES };
typedef struct { uint64_t start,last,phase[SJ_PHASES]; unsigned active,current; } sj_scope;
#ifdef _WIN32
static INIT_ONCE sj_once=INIT_ONCE_STATIC_INIT;
static SRWLOCK sj_lock=SRWLOCK_INIT;
static int sj_enabled;
static LONG64 sj_candidates;
static uint64_t sj_freq,sj_epoch,sj_total,sj_phase[SJ_PHASES],sj_samples,sj_failures,sj_last;
static unsigned sj_reports;
static BOOL CALLBACK sj_init(PINIT_ONCE a,PVOID b,PVOID* c) {
 (void)a;(void)b;(void)c; const char* e=getenv("SPURS_JOB_STAGE_PERF");
 sj_enabled=e && strcmp(e,"1")==0;
 if(sj_enabled){LARGE_INTEGER f; if(QueryPerformanceFrequency(&f) && f.QuadPart>0)sj_freq=f.QuadPart;else sj_enabled=0;}
 return TRUE;
}
static uint64_t sj_clock(void){LARGE_INTEGER q;QueryPerformanceCounter(&q);return q.QuadPart;}
static sj_scope sj_begin(int image) {
 sj_scope s={0};if(image!=10003)return s;
 InitOnceExecuteOnce(&sj_once,sj_init,NULL,NULL);if(!sj_enabled)return s;
 if(((uint64_t)InterlockedIncrement64(&sj_candidates)-1)%64)return s;
 s.active=1;s.current=SJ_ALLOC;s.start=s.last=sj_clock();return s;
}
static void sj_next(sj_scope* s,unsigned phase) {
 if(!s->active)return;uint64_t now=sj_clock();s->phase[s->current]+=now-s->last;s->last=now;s->current=phase;
}
static void sj_end(sj_scope* s,int failed) {
 if(!s->active)return;uint64_t now=sj_clock();s->phase[s->current]+=now-s->last;s->active=0;
 AcquireSRWLockExclusive(&sj_lock);
 if(!sj_samples)sj_epoch=sj_last=s->start;
 sj_samples++;sj_failures+=failed!=0;sj_total+=now-s->start;
 for(unsigned i=0;i<SJ_PHASES;i++)sj_phase[i]+=s->phase[i];
 if(now>=sj_last && now-sj_last>=sj_freq*5 && sj_reports<64){
  sj_last=now;sj_reports++;uint64_t sum=0;for(unsigned i=0;i<SJ_PHASES;i++)sum+=sj_phase[i];
  fprintf(stderr,"[spurs-job-stage] image=10003 every=64 samples=%llu failures=%llu total_ms=%.6f allocation_ms=%.6f staging_ms=%.6f context_init_copy_ms=%.6f guest_body_ms=%.6f snapshot_ms=%.6f cleanup_ms=%.6f phase_sum_ticks=%llu total_ticks=%llu report=%u scope=process_cumulative_sampled_wall_overlap\n",
  (unsigned long long)sj_samples,(unsigned long long)sj_failures,(double)sj_total*1000/sj_freq,
  (double)sj_phase[0]*1000/sj_freq,(double)sj_phase[1]*1000/sj_freq,(double)sj_phase[2]*1000/sj_freq,
  (double)sj_phase[3]*1000/sj_freq,(double)sj_phase[4]*1000/sj_freq,(double)sj_phase[5]*1000/sj_freq,
  (unsigned long long)sum,(unsigned long long)sj_total,sj_reports);
 }
 ReleaseSRWLockExclusive(&sj_lock);
}
#else
static sj_scope sj_begin(int image){(void)image;sj_scope s={0};return s;}
static void sj_next(sj_scope* s,unsigned phase){(void)s;(void)phase;}
static void sj_end(sj_scope* s,int failed){(void)s;(void)failed;}
#endif
#endif
