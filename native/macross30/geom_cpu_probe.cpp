// Selected actual10003 dispatch host threads. Diagnostic wall samples, not FPS.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <atomic>
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <map>
#include <string>
#include <thread>
#include <vector>
extern "C" { extern void (*g_spu_job_host_probe)(int,int,uint32_t); }
namespace {
struct Worker { HANDLE handle=nullptr; DWORD tid=0; uint64_t created=0; std::atomic<uint64_t> epoch{0}; std::atomic<uint32_t> desc{0}; };
std::array<Worker,32> workers; SRWLOCK registry=SRWLOCK_INIT; unsigned worker_count=0;
std::atomic<unsigned> overflow{0}; std::atomic<bool> finished{false};
thread_local Worker* local=nullptr; thread_local bool registered=false;
uint64_t ticks(FILETIME t){return (uint64_t(t.dwHighDateTime)<<32)|t.dwLowDateTime;}
bool cpu(HANDLE h,uint64_t&v){FILETIME c,e,k,u;if(!GetThreadTimes(h,&c,&e,&k,&u))return false;v=ticks(k)+ticks(u);return true;}
unsigned setting(const char*n,unsigned fallback,unsigned max){const char*v=getenv(n);if(!v||!*v)return fallback;char*e=nullptr;auto x=strtoul(v,&e,10);return e!=v&&!*e&&x<=max?unsigned(x):fallback;}
void hook(int image,int begin,uint32_t desc){
 if(image!=10003||finished.load(std::memory_order_relaxed))return;
 if(!registered){registered=true;AcquireSRWLockExclusive(&registry);
  if(worker_count<workers.size()){auto&w=workers[worker_count];HANDLE h=nullptr;if(DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),GetCurrentProcess(),&h,THREAD_QUERY_INFORMATION|THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT,FALSE,0)){w.handle=h;w.tid=GetCurrentThreadId();FILETIME c,e,k,u;if(GetThreadTimes(h,&c,&e,&k,&u))w.created=ticks(c);local=&w;++worker_count;}else overflow.fetch_add(1);}else overflow.fetch_add(1);
  ReleaseSRWLockExclusive(&registry);
 }
 if(!local)return;
 if(begin){local->desc.store(desc,std::memory_order_relaxed);local->epoch.fetch_add(1,std::memory_order_release);}else local->epoch.fetch_add(1,std::memory_order_release);
}
struct Bucket{unsigned count=0;std::string module;uint64_t rva=0,fn=0;bool unwind=false;};
struct Result{Worker*w=nullptr;unsigned attempts=0,inactive=0,suspended=0,contexts=0,changed=0,suspend_errors=0,context_errors=0,resume_errors=0;uint64_t begin=0,end=0,observed_from=0;bool begin_ok=false,end_ok=false;std::map<uint64_t,Bucket> buckets;};
void observe(std::string output,unsigned delay,unsigned duration,unsigned interval){
 Sleep(delay);const uint64_t started=GetTickCount64();std::vector<Result> results;results.reserve(32);
 while(GetTickCount64()-started<duration){
  AcquireSRWLockShared(&registry);unsigned n=worker_count;ReleaseSRWLockShared(&registry);
  while(results.size()<n){Result r;r.w=&workers[results.size()];r.observed_from=GetTickCount64();r.begin_ok=cpu(r.w->handle,r.begin);results.push_back(std::move(r));}
  for(auto&r:results){auto*w=r.w;auto epoch=w->epoch.load(std::memory_order_acquire);if(!(epoch&1)){++r.inactive;continue;}++r.attempts;
   CONTEXT ctx{};ctx.ContextFlags=CONTEXT_CONTROL;bool got=false,stable=false;
   if(SuspendThread(w->handle)!=DWORD(-1)){++r.suspended;got=GetThreadContext(w->handle,&ctx)!=0;if(!got)++r.context_errors;stable=w->epoch.load(std::memory_order_acquire)==epoch;
    // No locks, allocation, logging, module lookup, or stack walking while paused.
    if(ResumeThread(w->handle)==DWORD(-1)){++r.resume_errors;finished.store(true);break;}
   }else ++r.suspend_errors;
   if(got&&!stable){++r.changed;continue;}if(!got)continue;++r.contexts;
   auto&b=r.buckets[ctx.Rip];if(!b.count){HMODULE m=nullptr;if(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCSTR>(ctx.Rip),&m)){char name[MAX_PATH]{};GetModuleFileNameA(m,name,MAX_PATH);b.module=name;auto slash=b.module.find_last_of("\\/");if(slash!=std::string::npos)b.module.erase(0,slash+1);b.rva=ctx.Rip-reinterpret_cast<uint64_t>(m);DWORD64 image=0;auto*f=RtlLookupFunctionEntry(ctx.Rip,&image,nullptr);if(f){b.unwind=true;b.fn=image+f->BeginAddress-reinterpret_cast<uint64_t>(m);}}}++b.count;
  }
  if(finished.load())break;Sleep(interval);
 }
 finished.store(true);for(auto&r:results)r.end_ok=cpu(r.w->handle,r.end);
 std::ofstream f(output);f<<"{\"image\":10003,\"started_ms\":"<<started<<",\"ended_ms\":"<<GetTickCount64()<<",\"registration_failures_or_overflow\":"<<overflow.load()<<",\"workers\":[";bool first=true;
 for(auto&r:results){if(!first)f<<',';first=false;f<<"{\"host_thread\":"<<r.w->tid<<",\"creation_filetime\":"<<r.w->created<<",\"observed_from_ms\":"<<r.observed_from<<",\"attempts\":"<<r.attempts<<",\"inactive_polls\":"<<r.inactive<<",\"suspended\":"<<r.suspended<<",\"contexts\":"<<r.contexts<<",\"epoch_changed\":"<<r.changed<<",\"suspend_errors\":"<<r.suspend_errors<<",\"context_errors\":"<<r.context_errors<<",\"resume_errors\":"<<r.resume_errors<<",\"cpu_ms\":";if(r.begin_ok&&r.end_ok)f<<double(r.end-r.begin)/10000.;else f<<"null";f<<",\"buckets\":[";bool fb=true;for(auto&kv:r.buckets){auto&b=kv.second;if(!fb)f<<',';fb=false;f<<"{\"host_rip\":"<<kv.first<<",\"count\":"<<b.count<<",\"module\":"<<std::quoted(b.module)<<",\"rva\":"<<b.rva<<",\"unwind_extent_known\":"<<(b.unwind?"true":"false")<<",\"function_rva\":"<<b.fn<<'}';}f<<"],\"limits\":[]}";}
 f<<"],\"limits\":[\"Samples only when same actual10003 dispatch epoch spans suspension.\",\"Thread CPU includes other jobs between samples; not image-exclusive CPU.\",\"Retained handles plus creation timestamp disambiguate reused thread IDs.\",\"Inclusive wall RIP samples include waits, not CPU-weighted stacks; diagnostic perturbs scheduling.\",\"Exact RIP retained; unwind/MAP cannot identify inlined helper without suitable exact-build debug data.\"]}\n";
 // Retain handles until process exit: a concurrent hook may still own its slot.
}
}
extern "C" void macross_geom_cpu_probe_install(){
 const char*on=getenv("MACROSS_GEOM_CPU_PROBE"),*out=getenv("MACROSS_GEOM_CPU_PROBE_OUTPUT");if(!on||std::string(on)!="1"||!out||!*out)return;
 g_spu_job_host_probe=hook;
 std::thread(observe,std::string(out),setting("MACROSS_GEOM_CPU_PROBE_FROM_MS",80000,500000),setting("MACROSS_GEOM_CPU_PROBE_DURATION_MS",10000,60000),(std::max)(1u,setting("MACROSS_GEOM_CPU_PROBE_INTERVAL_MS",5,1000))).detach();
}
#else
extern "C" void macross_geom_cpu_probe_install(){}
#endif

