/* Opt-in shared-parent persistent job workers. No guest state/completion
 * fabrication. Caller keeps batch inputs/results alive until run_batch returns.
 * Fixed worker slot IDs share the same capacity with serial leases.
 */
#ifndef SPURS_JOB_PARALLEL_H
#define SPURS_JOB_PARALLEL_H
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#define SJP_MAX_SPUS 6u
#define SJP_MAX_BATCHES 32u
#ifndef SJP_CREATE_THREAD
#define SJP_CREATE_THREAD CreateThread
#endif
/* Diagnostic clocks are disabled by an immutable parent flag. No per-job
 * atomic RMW or QPC occurs when SPURS_POOL_DIAG is unset/0. */
static INIT_ONCE sjp_diag_once=INIT_ONCE_STATIC_INIT;
static int sjp_diag_on;static uint64_t sjp_diag_frequency;
static BOOL CALLBACK sjp_diag_init(PINIT_ONCE a,PVOID b,PVOID* c){
 (void)a;(void)b;(void)c;const char* e=getenv("SPURS_POOL_DIAG");
 sjp_diag_on=e&&strcmp(e,"1")==0;
 if(sjp_diag_on){LARGE_INTEGER f;if(QueryPerformanceFrequency(&f)&&f.QuadPart>0)sjp_diag_frequency=(uint64_t)f.QuadPart;else sjp_diag_on=0;}
 return TRUE;
}
static uint64_t sjp_diag_clock(void){LARGE_INTEGER q;QueryPerformanceCounter(&q);return (uint64_t)q.QuadPart;}
typedef struct {
 uint64_t waits,returns,signaled,timeouts,other_errors,claim_after_return;
 uint64_t no_work_signaled,no_work_timeout,no_work_other;
 uint64_t duty_samples,duty_ticks,idle_samples,idle_ticks,lock_samples,lock_ticks,lock_max_ticks;
} sjp_worker_diag;
typedef struct {
 sjp_worker_diag worker[SJP_MAX_SPUS];
 uint64_t owner_waits,owner_returns,owner_signaled,owner_timeouts,owner_not_ready_returns;
} sjp_diag_stats;
typedef struct sjp_parent sjp_parent;
typedef struct sjp_batch sjp_batch;
typedef struct {sjp_parent* parent;unsigned max_contention,eligible_mask,active;} sjp_chain;
typedef struct {int started,success;unsigned spu;unsigned long host_thread;uint32_t mailbox,intr,command;int mailbox_valid;} sjp_result;
typedef int (*sjp_run_fn)(void*,unsigned,unsigned,sjp_result*);
typedef void (*sjp_set_spu_fn)(unsigned);
typedef struct {sjp_parent* parent;unsigned slot;} sjp_worker_arg;
typedef struct {uint64_t batches,jobs_started,jobs_finished,serial_claims,hist[4];unsigned create_attempts,created,joined;} sjp_counters;
struct sjp_parent {
 SRWLOCK lock;CONDITION_VARIABLE changed;
 CONDITION_VARIABLE worker_ready[SJP_MAX_SPUS];
 unsigned capacity,used,users,closing;
 int pool_state,pool_abort;
 unsigned cursor;
 HANDLE workers[SJP_MAX_SPUS];sjp_worker_arg args[SJP_MAX_SPUS];
 sjp_batch* batches[SJP_MAX_BATCHES];
 sjp_counters counters;
 int diag_on;uint64_t diag_frequency;sjp_diag_stats diag;
 uint64_t diagnostic_last_ms;unsigned diagnostic_reports;
 volatile LONG work_seq;   /* bumped per posted batch; spinning workers watch it */
 uint64_t post_seq;        /* order batches were posted in (see sjp_worker) */
};
struct sjp_batch {
 sjp_chain* chain;unsigned count,next,active;int failed;
 CONDITION_VARIABLE done;
 volatile LONG* cancel;sjp_run_fn run;sjp_set_spu_fn set_spu;void* opaque;sjp_result* results;
 uint64_t seq;
 unsigned barred;   /* slots that took a newer batch's job while this one waited */
};
static void sjp_parent_init(sjp_parent* p,unsigned capacity){InitOnceExecuteOnce(&sjp_diag_once,sjp_diag_init,NULL,NULL);memset(p,0,sizeof(*p));p->diag_on=sjp_diag_on;p->diag_frequency=sjp_diag_frequency;InitializeSRWLock(&p->lock);InitializeConditionVariable(&p->changed);for(unsigned i=0;i<SJP_MAX_SPUS;i++)InitializeConditionVariable(&p->worker_ready[i]);p->capacity=capacity>SJP_MAX_SPUS?SJP_MAX_SPUS:capacity;}
/* Called only while parent predicate lock is held. */
static int sjp_stopped(sjp_batch* b){return b->failed||b->chain->parent->closing||(b->cancel&&InterlockedCompareExchange(b->cancel,0,0));}
/* Predicate lock is held. Only notify fixed workers with a currently
 * eligible free slot AND unclaimed work. A finishing worker can immediately
 * claim again, so do not notify its own empty CV. Notification is not a claim. */
static void sjp_notify_ready_locked(sjp_parent* p,unsigned avoid_slot){
 if(p->closing||p->pool_state!=2)return;
 unsigned free_slots=((1u<<p->capacity)-1)&~p->used, notify=0;
 if(avoid_slot<p->capacity)free_slots&=~(1u<<avoid_slot);
 if(!free_slots)return;
 for(unsigned i=0;i<SJP_MAX_BATCHES;i++){
  sjp_batch* b=p->batches[i];
  if(!b||sjp_stopped(b)||b->next>=b->count||b->chain->active>=b->chain->max_contention)continue;
  unsigned budget=b->chain->max_contention-b->chain->active, remaining=b->count-b->next;
  if(budget>remaining)budget=remaining;
  unsigned eligible=free_slots&b->chain->eligible_mask&~notify;
  for(unsigned slot=0;slot<p->capacity&&budget;slot++)if(eligible&(1u<<slot)){notify|=1u<<slot;budget--;}
 }
 for(unsigned slot=0;slot<p->capacity;slot++)if(notify&(1u<<slot))WakeConditionVariable(&p->worker_ready[slot]);
}
/* Short spin before sleeping. Geometry lists are small and very frequent;
 * a condition-variable round trip per list (wake a worker, then wake the
 * owner) left the workers ~40% idle between lists. SJP_NO_SPIN=1 disables. */
static int sjp_spin_on(void){static int on=-1;if(on<0)on=getenv("SJP_NO_SPIN")?0:1;return on;}
static int64_t sjp_spin_ticks(unsigned us){static int64_t f;if(!f){LARGE_INTEGER q;QueryPerformanceFrequency(&q);f=q.QuadPart;}return f*(int64_t)us/1000000;}
static int64_t sjp_now(void){LARGE_INTEGER q;QueryPerformanceCounter(&q);return q.QuadPart;}
static void sjp_wake_all_workers_locked(sjp_parent* p){for(unsigned i=0;i<p->capacity;i++)WakeAllConditionVariable(&p->worker_ready[i]);}
static DWORD WINAPI sjp_worker(LPVOID arg){
 sjp_worker_arg* a=(sjp_worker_arg*)arg;sjp_parent* p=a->parent;unsigned slot=a->slot;
 uint64_t duty_sequence=0,idle_sequence=0;
 int returned=0,returned_kind=0;
 AcquireSRWLockExclusive(&p->lock);
 for(;;){
  if(p->closing||p->pool_abort){ReleaseSRWLockExclusive(&p->lock);return 0;}
  sjp_batch* b=NULL;
  if(p->pool_state==2&&!(p->used&(1u<<slot))){
   /* Oldest posted batch first, and never a newer one while an older batch
    * this slot may take still has unclaimed jobs. A slot is an SPU number:
    * titles key per-SPU output rings on it (Edge geometry), filled in the
    * order the slot runs jobs and drained by the GPU in command-buffer
    * order. Taking a newer list's jobs and then an older list's left a job
    * waiting for ring space only a later command-buffer hole frees -- a hang,
    * and ring data overwritten before it was drawn.
    * An older batch held back by its chain's contention limit may be passed
    * over -- the slot is then barred from it, so it never runs that batch's
    * remaining jobs after the newer ones -- unless that would leave the older
    * batch no slot at all; then the worker waits for it. */
   sjp_batch* skipped[SJP_MAX_BATCHES];unsigned n_skipped=0;uint64_t after=0;
   static int order=-1;if(order<0){const char* e=getenv("SJP_ORDER");order=!(e&&*e=='0');}
   if(!order){   /* SJP_ORDER=0: the earlier round-robin pick (A/B only) */
    for(unsigned n=0;n<SJP_MAX_BATCHES&&!b;n++){
     unsigned i=(p->cursor+n)%SJP_MAX_BATCHES;sjp_batch* c=p->batches[i];
     if(c&&!sjp_stopped(c)&&c->next<c->count&&(c->chain->eligible_mask&(1u<<slot))&&c->chain->active<c->chain->max_contention){b=c;p->cursor=(i+1)%SJP_MAX_BATCHES;}
    }
   }
   for(;order;){
    sjp_batch* next=NULL;
    for(unsigned i=0;i<SJP_MAX_BATCHES;i++){
     sjp_batch* candidate=p->batches[i];
     if(candidate&&candidate->seq>after&&!sjp_stopped(candidate)&&candidate->next<candidate->count&&
        (candidate->chain->eligible_mask&(1u<<slot))&&!(candidate->barred&(1u<<slot))&&
        (!next||candidate->seq<next->seq))next=candidate;
    }
    if(!next)break;
    if(next->chain->active<next->chain->max_contention){b=next;break;}
    const unsigned others=next->chain->eligible_mask&~next->barred&~(1u<<slot)&((1u<<p->capacity)-1);
    if(!others)break;   /* this slot is the older batch's last one: wait for it */
    skipped[n_skipped++]=next;after=next->seq;
   }
   if(b)for(unsigned k=0;k<n_skipped;k++)skipped[k]->barred|=1u<<slot;
  }
  if(!b){
   uint64_t before=0;int sample=0;
   if(p->diag_on){
    sjp_worker_diag* d=&p->diag.worker[slot];
    if(returned){if(returned_kind==1)d->no_work_signaled++;else if(returned_kind==2)d->no_work_timeout++;else d->no_work_other++;}
    d->waits++;sample=(idle_sequence++%64)==0;if(sample)before=sjp_diag_clock();
   }
   if(sjp_spin_on()){
    const LONG seen=p->work_seq;const int64_t until=sjp_now()+sjp_spin_ticks(50);
    ReleaseSRWLockExclusive(&p->lock);
    while(p->work_seq==seen&&!p->closing&&sjp_now()<until)YieldProcessor();
    AcquireSRWLockExclusive(&p->lock);
    if(p->work_seq!=seen)continue;
   }
   BOOL woke=SleepConditionVariableSRW(&p->worker_ready[slot],&p->lock,10,0);
   DWORD error=(!woke&&p->diag_on)?GetLastError():0;
   if(p->diag_on){
    sjp_worker_diag* d=&p->diag.worker[slot];d->returns++;
    if(woke)d->signaled++;else if(error==ERROR_TIMEOUT)d->timeouts++;else d->other_errors++;
    if(sample){d->idle_samples++;d->idle_ticks+=sjp_diag_clock()-before;}
    returned=1;returned_kind=woke?1:error==ERROR_TIMEOUT?2:3;
   }
   continue;
  }
  if(p->diag_on&&returned){p->diag.worker[slot].claim_after_return++;returned=0;}
  int duty_sample=p->diag_on&&((duty_sequence++%64)==0);
  unsigned index=b->next++;b->active++;b->chain->active++;p->used|=1u<<slot;p->counters.jobs_started++;
  sjp_result* r=&b->results[index];r->started=1;r->spu=slot;r->host_thread=GetCurrentThreadId();
  ReleaseSRWLockExclusive(&p->lock);
  uint64_t duty_before=duty_sample?sjp_diag_clock():0;
  if(b->set_spu)b->set_spu(slot);
  int ok=b->run(b->opaque,index,slot,r);r->success=ok!=0;
  uint64_t lock_before=duty_sample?sjp_diag_clock():0;
  AcquireSRWLockExclusive(&p->lock);
  if(duty_sample){
   uint64_t after=sjp_diag_clock(),wait=after-lock_before;sjp_worker_diag* d=&p->diag.worker[slot];
   d->duty_samples++;d->duty_ticks+=lock_before-duty_before;d->lock_samples++;d->lock_ticks+=wait;if(wait>d->lock_max_ticks)d->lock_max_ticks=wait;
  }
  if(!ok)b->failed=1;
  b->active--;b->chain->active--;p->used&=~(1u<<slot);p->counters.jobs_finished++;
  sjp_notify_ready_locked(p,slot);
  if(!b->active&&(sjp_stopped(b)||b->next==b->count))WakeConditionVariable(&b->done);
  /* Only serial slot/control waiters use changed; workers and batch owners
   * no longer wake on this capacity notification. */
  WakeAllConditionVariable(&p->changed);
 }
}
/* One initializer; other batch owners/close wait for its complete cleanup. */
static int sjp_start_pool(sjp_parent* p){
 AcquireSRWLockExclusive(&p->lock);
 while(p->pool_state==1)SleepConditionVariableSRW(&p->changed,&p->lock,INFINITE,0);
 if(p->closing||p->pool_state<0){ReleaseSRWLockExclusive(&p->lock);return 0;}
 if(p->pool_state==2){ReleaseSRWLockExclusive(&p->lock);return 1;}
 p->pool_state=1;ReleaseSRWLockExclusive(&p->lock);
 unsigned made=0;int failed=0;
 for(unsigned i=0;i<p->capacity;i++){
  AcquireSRWLockExclusive(&p->lock);
  if(p->closing){ReleaseSRWLockExclusive(&p->lock);failed=1;break;}
  p->args[i].parent=p;p->args[i].slot=i;p->counters.create_attempts++;
  ReleaseSRWLockExclusive(&p->lock);
  HANDLE h=SJP_CREATE_THREAD(NULL,1u<<20,sjp_worker,&p->args[i],0,NULL);
  if(!h){failed=1;break;}
  p->workers[made++]=h;
 }
 AcquireSRWLockExclusive(&p->lock);
 p->counters.created+=made;
 if(p->closing)failed=1;
 if(!failed){p->pool_state=2;WakeAllConditionVariable(&p->changed);ReleaseSRWLockExclusive(&p->lock);return 1;}
 p->pool_abort=1;sjp_wake_all_workers_locked(p);WakeAllConditionVariable(&p->changed);ReleaseSRWLockExclusive(&p->lock);
 for(unsigned i=0;i<made;i++){WaitForSingleObject(p->workers[i],INFINITE);CloseHandle(p->workers[i]);p->workers[i]=NULL;}
 AcquireSRWLockExclusive(&p->lock);p->counters.joined+=made;p->pool_state=-1;WakeAllConditionVariable(&p->changed);ReleaseSRWLockExclusive(&p->lock);return 0;
}
static int sjp_run_batch(sjp_chain* chain,unsigned count,sjp_result* results,volatile LONG* cancel,sjp_run_fn run,sjp_set_spu_fn set_spu,void* opaque){
 if(!chain||!chain->parent||!run||(!results&&count))return 0;
 sjp_parent* p=chain->parent;
 unsigned eligible=chain->eligible_mask&((1u<<p->capacity)-1);
 if(!p->capacity||!chain->max_contention||!eligible)return 0;
 if(!count)return 1;
 memset(results,0,sizeof(*results)*count);
 if(!sjp_start_pool(p))return 0;
 sjp_batch b;memset(&b,0,sizeof b);b.chain=chain;b.count=count;b.cancel=cancel;b.run=run;b.set_spu=set_spu;b.opaque=opaque;b.results=results;InitializeConditionVariable(&b.done);
 AcquireSRWLockExclusive(&p->lock);
 if(p->closing){ReleaseSRWLockExclusive(&p->lock);return 0;}
 unsigned qi=0;while(qi<SJP_MAX_BATCHES&&p->batches[qi])qi++;
 if(qi==SJP_MAX_BATCHES){ReleaseSRWLockExclusive(&p->lock);return 0;}
 b.seq=++p->post_seq;b.barred=0;p->batches[qi]=&b;p->users++;p->counters.batches++;InterlockedIncrement(&p->work_seq);p->counters.hist[count<=4?0:count<=31?1:count<=127?2:3]++;
 sjp_notify_ready_locked(p,SJP_MAX_SPUS);
 while(b.active||(!sjp_stopped(&b)&&b.next<count)){
  if(sjp_spin_on()){
   const int64_t until=sjp_now()+sjp_spin_ticks(200);
   ReleaseSRWLockExclusive(&p->lock);
   while((*(volatile unsigned*)&b.active||*(volatile unsigned*)&b.next<count)&&!*(volatile int*)&b.failed&&sjp_now()<until)YieldProcessor();
   AcquireSRWLockExclusive(&p->lock);
   if(!(b.active||(!sjp_stopped(&b)&&b.next<count)))break;
  }
  if(p->diag_on)p->diag.owner_waits++;
  BOOL woke=SleepConditionVariableSRW(&b.done,&p->lock,10,0);DWORD error=(!woke&&p->diag_on)?GetLastError():0;
  if(p->diag_on){p->diag.owner_returns++;if(woke)p->diag.owner_signaled++;else if(error==ERROR_TIMEOUT)p->diag.owner_timeouts++;
   if(b.active||(!sjp_stopped(&b)&&b.next<count))p->diag.owner_not_ready_returns++;
  }
 }
 int ok=!sjp_stopped(&b)&&b.next==count;
 p->batches[qi]=NULL;p->users--;WakeAllConditionVariable(&p->changed);ReleaseSRWLockExclusive(&p->lock);
 return ok;
}
/* Split form of sjp_run_batch: submit returns once the batch is queued, so
 * the chain walker can go on to the next command (the next JOBLIST the game
 * is still streaming in) while workers run this one; wait joins it. Same
 * queueing, claiming and completion rules as sjp_run_batch. */
typedef struct sjp_async { sjp_batch b; unsigned qi; } sjp_async;
static sjp_async* sjp_submit_batch(sjp_chain* chain,unsigned count,sjp_result* results,volatile LONG* cancel,sjp_run_fn run,sjp_set_spu_fn set_spu,void* opaque){
 if(!chain||!chain->parent||!run||!results||!count)return NULL;
 sjp_parent* p=chain->parent;
 unsigned eligible=chain->eligible_mask&((1u<<p->capacity)-1);
 if(!p->capacity||!chain->max_contention||!eligible)return NULL;
 memset(results,0,sizeof(*results)*count);
 if(!sjp_start_pool(p))return NULL;
 sjp_async* a=(sjp_async*)calloc(1,sizeof *a);if(!a)return NULL;
 sjp_batch* b=&a->b;b->chain=chain;b->count=count;b->cancel=cancel;b->run=run;b->set_spu=set_spu;b->opaque=opaque;b->results=results;InitializeConditionVariable(&b->done);
 AcquireSRWLockExclusive(&p->lock);
 if(p->closing){ReleaseSRWLockExclusive(&p->lock);free(a);return NULL;}
 unsigned qi=0;while(qi<SJP_MAX_BATCHES&&p->batches[qi])qi++;
 if(qi==SJP_MAX_BATCHES){ReleaseSRWLockExclusive(&p->lock);free(a);return NULL;}
 b->seq=++p->post_seq;b->barred=0;a->qi=qi;p->batches[qi]=b;p->users++;p->counters.batches++;InterlockedIncrement(&p->work_seq);p->counters.hist[count<=4?0:count<=31?1:count<=127?2:3]++;
 sjp_notify_ready_locked(p,SJP_MAX_SPUS);
 ReleaseSRWLockExclusive(&p->lock);
 return a;
}
static int sjp_wait_batch(sjp_async* a){
 sjp_batch* b=&a->b;sjp_parent* p=b->chain->parent;const unsigned count=b->count;
 AcquireSRWLockExclusive(&p->lock);
 while(b->active||(!sjp_stopped(b)&&b->next<count)){
  if(sjp_spin_on()){
   const int64_t until=sjp_now()+sjp_spin_ticks(200);
   ReleaseSRWLockExclusive(&p->lock);
   while((*(volatile unsigned*)&b->active||*(volatile unsigned*)&b->next<count)&&!*(volatile int*)&b->failed&&sjp_now()<until)YieldProcessor();
   AcquireSRWLockExclusive(&p->lock);
   if(!(b->active||(!sjp_stopped(b)&&b->next<count)))break;
  }
  SleepConditionVariableSRW(&b->done,&p->lock,10,0);
 }
 int ok=!sjp_stopped(b)&&b->next==count;
 p->batches[a->qi]=NULL;p->users--;WakeAllConditionVariable(&p->changed);ReleaseSRWLockExclusive(&p->lock);
 free(a);
 return ok;
}
/* Serial fallback reserves a shared identity without spawning any thread. */
static int sjp_claim_serial(sjp_chain* chain,unsigned slot,int (*cancel)(void*),void* opaque){
 if(!chain||!chain->parent||slot>=chain->parent->capacity||!(chain->eligible_mask&(1u<<slot)))return 0;
 sjp_parent* p=chain->parent;
 for(;;){
  if(cancel&&cancel(opaque))return 0;
  AcquireSRWLockExclusive(&p->lock);
  if(p->closing){ReleaseSRWLockExclusive(&p->lock);return 0;}
  if(!(p->used&(1u<<slot))&&chain->active<chain->max_contention){p->used|=1u<<slot;p->users++;chain->active++;p->counters.serial_claims++;ReleaseSRWLockExclusive(&p->lock);return 1;}
  SleepConditionVariableSRW(&p->changed,&p->lock,10,0);ReleaseSRWLockExclusive(&p->lock);
 }
}
/* Choose a free eligible identity atomically; prefer high IDs so a 1F
 * serial chain can use slot4 while a 0F geometry chain uses slots0..3.
 * Identity remains fixed until the matching release. */
static int sjp_claim_serial_any(sjp_chain* chain,unsigned* slot_out,int (*cancel)(void*),void* opaque){
 if(!chain||!chain->parent||!slot_out)return 0;
 sjp_parent* p=chain->parent;
 unsigned mask=chain->eligible_mask&((1u<<p->capacity)-1);
 if(!mask||!chain->max_contention)return 0;
 for(;;){
  if(cancel&&cancel(opaque))return 0;
  AcquireSRWLockExclusive(&p->lock);
  if(p->closing){ReleaseSRWLockExclusive(&p->lock);return 0;}
  if(chain->active<chain->max_contention){
   for(unsigned n=p->capacity;n>0;n--){unsigned slot=n-1;
    if((mask&(1u<<slot))&&!(p->used&(1u<<slot))){p->used|=1u<<slot;p->users++;chain->active++;p->counters.serial_claims++;*slot_out=slot;ReleaseSRWLockExclusive(&p->lock);return 1;}
   }
  }
  SleepConditionVariableSRW(&p->changed,&p->lock,10,0);ReleaseSRWLockExclusive(&p->lock);
 }
}
static void sjp_release_serial(sjp_chain* chain,unsigned slot){sjp_parent* p=chain->parent;AcquireSRWLockExclusive(&p->lock);p->used&=~(1u<<slot);p->users--;chain->active--;sjp_notify_ready_locked(p,SJP_MAX_SPUS);WakeAllConditionVariable(&p->changed);ReleaseSRWLockExclusive(&p->lock);}
static void sjp_get_counters(sjp_parent* p,sjp_counters* out){AcquireSRWLockShared(&p->lock);*out=p->counters;ReleaseSRWLockShared(&p->lock);}
static void sjp_parent_close(sjp_parent* p){
 HANDLE threads[SJP_MAX_SPUS]={0};
 AcquireSRWLockExclusive(&p->lock);p->closing=1;WakeAllConditionVariable(&p->changed);sjp_wake_all_workers_locked(p);
 for(unsigned i=0;i<SJP_MAX_BATCHES;i++)if(p->batches[i])WakeAllConditionVariable(&p->batches[i]->done);
 while(p->pool_state==1||p->users)SleepConditionVariableSRW(&p->changed,&p->lock,INFINITE,0);
 for(unsigned i=0;i<p->capacity;i++){threads[i]=p->workers[i];p->workers[i]=NULL;}
 ReleaseSRWLockExclusive(&p->lock);
 unsigned joined=0;for(unsigned i=0;i<p->capacity;i++)if(threads[i]){WaitForSingleObject(threads[i],INFINITE);CloseHandle(threads[i]);joined++;}
 AcquireSRWLockExclusive(&p->lock);p->counters.joined+=joined;ReleaseSRWLockExclusive(&p->lock);
}
#endif
