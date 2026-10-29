// Bounded actualfunctionentry/return observation. Never changesgueststate.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "geom_c5b0_capture.h"
#include "geom_c5b0_capture_contract.h"
extern "C" { int g_macross_geom_c5b0_capture_on=0; }
namespace {
std::string directory;std::atomic<unsigned> reserved{0},errors{0};
std::string gate_path;std::atomic<bool> gate_armed{true};std::atomic<uint64_t> gate_check_after{0};
bool gate_ready(){
 if(gate_armed.load(std::memory_order_acquire))return true;
 const uint64_t now=GetTickCount64();uint64_t next=gate_check_after.load(std::memory_order_relaxed);
 if(now>=next&&gate_check_after.compare_exchange_strong(next,now+250,std::memory_order_relaxed)){
  DWORD a=GetFileAttributesA(gate_path.c_str());if(a!=INVALID_FILE_ATTRIBUTES&&!(a&FILE_ATTRIBUTE_DIRECTORY))gate_armed.store(true,std::memory_order_release);
 }
 return gate_armed.load(std::memory_order_acquire);
}
thread_local unsigned local_count=0;thread_local int local_tokens[2]={-1,-1};
void error(const char*what){if(errors.fetch_add(1)<8)fprintf(stderr,"[geom-c5b0-capture] write_failed=%s error=%lu\n",what,(unsigned long)GetLastError());}
bool write_file(const std::string&p,const void*data,size_t n){FILE*f=fopen(p.c_str(),"wb");if(!f){error(p.c_str());return false;}bool ok=fwrite(data,1,n,f)==n;ok=fclose(f)==0&&ok;if(!ok)error(p.c_str());return ok;}
std::string stem(int token){char x[80];snprintf(x,sizeof x,"/c5b0_%02d_t%lu",token,(unsigned long)GetCurrentThreadId());return directory+x;}
bool snapshot(spu_context*c,int token,const char*phase){const auto p=stem(token)+"."+phase;bool ok=true;ok=write_file(p+".ls.bin",c->ls,SPU_LS_SIZE)&&ok;ok=write_file(p+".gpr.bin",c->gpr,sizeof c->gpr)&&ok;ok=write_file(p+".context.bin",c,sizeof *c)&&ok;
 char json[800];int n=snprintf(json,sizeof json,"{\"phase\":\"%s\",\"logical_entry\":50608,\"image\":%d,\"actual_pc\":%u,\"status\":%u,\"stop_code\":%u,\"host_depth\":%u,\"guest_lr\":%u,\"thread\":%lu,\"monotonic_ms\":%llu,\"raw_context_bytes\":%zu,\"raw_context_pointers_restorable\":false,\"data_files_complete\":%s}\n",phase,c->image_id,(unsigned)c->pc,(unsigned)c->status,c->stop_code,c->host_depth,c->gpr[0]._u32[0],(unsigned long)GetCurrentThreadId(),(unsigned long long)GetTickCount64(),sizeof *c,ok?"true":"false");ok=write_file(p+".json",json,(size_t)n)&&ok;return ok;}
}
extern "C" void macross_geom_c5b0_capture_install(){
 const char*on=getenv("MACROSS_GEOM_C5B0_CAPTURE"),*path=getenv("MACROSS_GEOM_C5B0_CAPTURE_DIR");if(!on||strcmp(on,"1")||!path||!*path)return;
 char exe[MAX_PATH],root[MAX_PATH],full[MAX_PATH];DWORD n=GetModuleFileNameA(nullptr,exe,MAX_PATH);if(!n||n>=MAX_PATH)return;char*slash=strrchr(exe,'\\');if(!slash)return;*slash=0;std::string base=std::string(exe)+"/../run/";
 const DWORD root_n=GetFullPathNameA(base.c_str(),MAX_PATH,root,nullptr),full_n=GetFullPathNameA(path,MAX_PATH,full,nullptr);
 if(!root_n||root_n>=MAX_PATH||!full_n||full_n>=MAX_PATH)return;
 std::string canonicalRoot=root;for(char&c:canonicalRoot)if(c=='/')c='\\';for(char*p=full;*p;p++)if(*p=='/')*p='\\';if(canonicalRoot.back()!='\\')canonicalRoot+='\\';
 if(_strnicmp(full,canonicalRoot.c_str(),canonicalRoot.size())||strlen(full)<=canonicalRoot.size()){fprintf(stderr,"[geom-c5b0-capture] rejected_dir_outside_native_run\n");return;}
 // Require a new immediatechild directory. Existing/reparseparent paths rejected.
 const char*leaf=full+canonicalRoot.size();if(strchr(leaf,'\\')||!strcmp(leaf,".")||!strcmp(leaf,".."))return;
 DWORD attr=GetFileAttributesA(root);if(attr==INVALID_FILE_ATTRIBUTES||(attr&FILE_ATTRIBUTE_REPARSE_POINT))return;
 if(!CreateDirectoryA(full,nullptr)){fprintf(stderr,"[geom-c5b0-capture] rejected_dir_not_fresh error=%lu\n",(unsigned long)GetLastError());return;}
 directory=full;const char*gate=getenv("MACROSS_GEOM_C5B0_CAPTURE_GATE");if(gate&&*gate){gate_path=gate;gate_armed.store(false);}
 g_macross_geom_c5b0_capture_on=1;fprintf(stderr,"[geom-c5b0-capture] enabled dir=%s max_pairs=8 per_thread=2 gate=%s\n",full,gate_path.empty()?"none":gate_path.c_str());
}
extern "C" int macross_geom_c5b0_capture_pre(spu_context*c){
 if(!c||!c->ls||c->image_id!=10003||local_count>=2||reserved.load(std::memory_order_relaxed)>=8)return -1;
 if(!gate_ready())return -1;
 if(memcmp(c->ls+0xC5B0,expected_c5b0,sizeof expected_c5b0)||memcmp(c->ls+0x88E0,expected_88e0,sizeof expected_88e0))return -1;
 unsigned token=reserved.fetch_add(1);if(token>=8)return -1;local_tokens[local_count++]=(int)token;
 const char marker[]="{\"post_return_observed\":false,\"meaning\":\"pre_only_until_real_function_returns\"}\n";
 write_file(stem(token)+".pair.json",marker,sizeof marker-1);snapshot(c,token,"pre");return (int)token;
}
extern "C" void macross_geom_c5b0_capture_post(spu_context*c,int token){
 if(token<0||token>=8)return;bool own=false;for(unsigned i=0;i<local_count;i++)if(local_tokens[i]==token){own=true;local_tokens[i]=-1;break;}if(!own)return;
 bool ok=snapshot(c,token,"post");char marker[180];int n=snprintf(marker,sizeof marker,"{\"post_return_observed\":true,\"post_data_files_complete\":%s,\"guest_job_completion_claimed\":false}\n",ok?"true":"false");write_file(stem(token)+".pair.json",marker,(size_t)n);
}
