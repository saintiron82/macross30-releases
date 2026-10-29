/* Opt-in posttask boundary CPU profiling. Included by guard.c only. */
#ifndef MACROSS_POSTTASK_BOUNDARY_PERF_H
#define MACROSS_POSTTASK_BOUNDARY_PERF_H
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
static const int ptp_families[]={0,1,2,4,6,8,9,10,11};
#define PTP_NATIVE_COUNT (sizeof(ptp_families)/sizeof(ptp_families[0]))
#define PTP_UNKNOWN_CAP 16
#define PTP_UNKNOWN_BASE PTP_NATIVE_COUNT
#define PTP_PC_OVERFLOW (PTP_UNKNOWN_BASE+PTP_UNKNOWN_CAP)
#define PTP_DEPTH_OVERFLOW (PTP_PC_OVERFLOW+1)
#define PTP_BUCKETS (PTP_DEPTH_OVERFLOW+1)
#define PTP_REPORT_CAP 128
#define PTP_DEPTH 16
typedef struct { uint64_t wall,cpu; int cpu_ok; } ptp_stamp;
typedef struct { uint64_t calls,wall,self_wall,cpu,self_cpu,fail; uint32_t pc; int family; } ptp_bucket;
typedef struct { ptp_stamp start; uint64_t child_wall,child_cpu; int bucket,child_fail; } ptp_frame;
typedef struct { ptp_frame stack[PTP_DEPTH]; ptp_bucket bucket[PTP_BUCKETS]; unsigned depth,overflow_depth,overflow_count; ptp_frame overflow; uint64_t frequency,last_report; unsigned unknown_count; } ptp_thread;
static __declspec(thread) ptp_thread ptp;
static INIT_ONCE ptp_init_once=INIT_ONCE_STATIC_INIT;
static int ptp_enabled;
static volatile LONG ptp_reports;
static BOOL CALLBACK ptp_init(PINIT_ONCE a,PVOID b,PVOID*c){const char*e=getenv("MACROSS_POSTTASK_PERF");ptp_enabled=e&&atoi(e)!=0;return TRUE;}
static ptp_stamp ptp_now(void){LARGE_INTEGER q;FILETIME c,e,k,u;ptp_stamp s={0};QueryPerformanceCounter(&q);s.wall=q.QuadPart;if(GetThreadTimes(GetCurrentThread(),&c,&e,&k,&u)){s.cpu=((uint64_t)k.dwHighDateTime<<32)|k.dwLowDateTime;s.cpu+=((uint64_t)u.dwHighDateTime<<32)|u.dwLowDateTime;s.cpu_ok=1;}return s;}
static int ptp_family_bucket(int family){for(unsigned i=0;i<PTP_NATIVE_COUNT;i++)if(ptp_families[i]==family)return (int)i;return -1;}
static int ptp_bucket_for(uint32_t pc,int family){int k=ptp_family_bucket(family);if(k>=0){ptp.bucket[k].family=family;ptp.bucket[k].pc=pc;return k;}for(unsigned i=0;i<ptp.unknown_count;i++)if(ptp.bucket[PTP_UNKNOWN_BASE+i].pc==pc)return PTP_UNKNOWN_BASE+i;if(ptp.unknown_count<PTP_UNKNOWN_CAP){k=PTP_UNKNOWN_BASE+ptp.unknown_count++;ptp.bucket[k].family=-1;ptp.bucket[k].pc=pc;return k;}ptp.bucket[PTP_PC_OVERFLOW].family=-2;return PTP_PC_OVERFLOW;}
/* Pure accounting helper: fixtures exercise exact nested clock schedules. */
static void ptp_account(ptp_frame*f,ptp_stamp end){
 ptp_bucket*b=&ptp.bucket[f->bucket];uint64_t w=end.wall>=f->start.wall?end.wall-f->start.wall:0;
 int valid=f->start.cpu_ok&&end.cpu_ok&&end.cpu>=f->start.cpu&&!f->child_fail;
 uint64_t c=valid?end.cpu-f->start.cpu:0;
 b->calls++;b->wall+=w;b->self_wall+=w>=f->child_wall?w-f->child_wall:0;
 if(valid){b->cpu+=c;b->self_cpu+=c>=f->child_cpu?c-f->child_cpu:0;}else b->fail++;
 if(ptp.depth){ptp_frame*p=&ptp.stack[ptp.depth-1];p->child_wall+=w;p->child_cpu+=c;if(!valid)p->child_fail=1;}
}
static void ptp_report(ptp_stamp now){
 if(ptp.depth||!ptp.frequency||now.wall-ptp.last_report<5*ptp.frequency)return;
 if(InterlockedCompareExchange(&ptp_reports,0,0)>=PTP_REPORT_CAP)return;
 LONG report=InterlockedIncrement(&ptp_reports);if(report>PTP_REPORT_CAP)return;
 fprintf(stderr,"[posttask-perf] host=%lu report=%ld span_ms=%.3f depth_overflow=%u (per-thread interval; inclusive overlaps, self excludes children)\n",GetCurrentThreadId(),report,1000.0*(now.wall-ptp.last_report)/ptp.frequency,ptp.overflow_count);
 for(int i=0;i<PTP_BUCKETS;i++){ptp_bucket*b=&ptp.bucket[i];if(!b->calls)continue;fprintf(stderr,"[posttask-perf] host=%lu family=%d pc=%05X kind=%s calls=%llu wall_ms=%.3f self_wall_ms=%.3f cpu_ms=%.3f self_cpu_ms=%.3f cpu_fail=%llu\n",GetCurrentThreadId(),b->family,b->pc,i<PTP_NATIVE_COUNT?"native":i==PTP_DEPTH_OVERFLOW?"depth-overflow":i==PTP_PC_OVERFLOW?"pc-overflow":"interpreter",(unsigned long long)b->calls,1000.0*b->wall/ptp.frequency,1000.0*b->self_wall/ptp.frequency,b->cpu/10000.0,b->self_cpu/10000.0,(unsigned long long)b->fail);memset(b,0,sizeof(*b));}
 ptp.unknown_count=0;ptp.overflow_count=0;ptp.last_report=now.wall;
}
int macross_posttask_perf_begin(spu_context*ctx,uint32_t pc,int family){
 if(ctx->image_id!=318051041)return 0;
 InitOnceExecuteOnce(&ptp_init_once,ptp_init,NULL,NULL);if(!ptp_enabled)return 0;
 ptp_stamp now=ptp_now();if(!ptp.frequency){LARGE_INTEGER f;QueryPerformanceFrequency(&f);ptp.frequency=f.QuadPart;ptp.last_report=now.wall;}
 if(ptp.depth>=PTP_DEPTH||ptp.overflow_depth){if(!ptp.overflow_depth){memset(&ptp.overflow,0,sizeof(ptp.overflow));ptp.overflow.start=now;ptp.overflow.bucket=PTP_DEPTH_OVERFLOW;ptp.bucket[PTP_DEPTH_OVERFLOW].family=-3;}ptp.overflow_depth++;ptp.overflow_count++;return -1;}
 ptp_frame*f=&ptp.stack[ptp.depth++];memset(f,0,sizeof(*f));f->start=now;f->bucket=ptp_bucket_for(pc,family);return (int)ptp.depth;
}
void macross_posttask_perf_end(int token){
 if(!token)return;ptp_stamp now=ptp_now();
 if(token<0){if(ptp.overflow_depth&&!--ptp.overflow_depth)ptp_account(&ptp.overflow,now);return;}
 if((unsigned)token!=ptp.depth)return;ptp_frame f=ptp.stack[--ptp.depth];ptp_account(&f,now);ptp_report(now);
}
#endif
