"""Prepare isolated ORIGINAL geom1 C5B0 CPU oracle source. Never compiles/runs.
No production hook is installed. Input LS/GPR must be captured atactualC5B0entry,
not job initialization. Unsupported control transfers fail ratherthancomplete.
"""
from pathlib import Path
import re,json,hashlib,argparse
R=Path(__file__).resolve().parents[2]
def body(s,name):
 a=s.index('void macross_geom1_spu_func_'+name+'(');b=s.index('\nvoid ',a+1);return s[a:b]
def prepare(out):
 import sys as _sys, pathlib as _pl; _sys.path[:0] = [str(_d) for _d in sorted(_pl.Path(__file__).resolve().parents[1].iterdir()) if _d.is_dir()]  # tools/* siblings
 from patch_geom_c5b0_capture import original_source
 source=original_source((R/'native/macross30/spu_gameplay/geom1/spu_recomp.c').read_text());f=body(source,'0000C5B0');helper=body(source,'000088E0')
 assert sorted(set(re.findall(r'macross_geom1_spu_func_([0-9A-F]+)\(ctx\)',f)))==['000088E0']
 assert not re.search(r'spu_(?:rdch|wrch|stop|halt|indirect_branch)\(',f+helper)
 prefix=r'''#include "spu_context.h"
#include "spu_helpers.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);
int g_spu_ls_watch_n=0;
void spu_ls_watch_slow(uint32_t a,int b,const uint8_t*c,uint32_t d,uint32_t e){abort();}
void spu_indirect_branch(spu_context*c){abort();}
void spu_drain_call(spu_context*c,uint32_t ret){assert(!g_spu_trampoline_fn);assert(c->pc==ret);}
void spu_img_restore(spu_context*c,int32_t image){assert(c->image_id==image);}
void spu_stop(spu_context*c){abort();}
void spu_halt(spu_context*c){abort();}
void macross_geom1_spu_func_0000D430(spu_context*c){abort();}
void macross_geom1_spu_func_000088F8(spu_context*c){abort();}
void macross_geom1_spu_func_000088E0(spu_context*);
static void load(const char*p,void*d,size_t n){FILE*f=fopen(p,"rb");assert(f);assert(fread(d,1,n,f)==n);assert(fgetc(f)==EOF);fclose(f);}
'''
 suffix=r'''
int main(int argc,char**argv){assert(argc==5);static spu_context c;load(argv[4],&c,sizeof c);c.ls=c.ls_store;load(argv[1],c.ls,SPU_LS_SIZE);load(argv[2],c.gpr,sizeof c.gpr);assert(c.image_id==10003&&c.pc==0xC5B0&&c.host_depth==1);g_spu_trampoline_fn=0;uint32_t initial_status=c.status;
macross_geom1_spu_func_0000C5B0(&c);assert(!g_spu_trampoline_fn);assert(c.status==initial_status);assert(c.host_depth==1);FILE*f=fopen(argv[3],"wb");assert(f);assert(fwrite(c.ls,1,SPU_LS_SIZE,f)==SPU_LS_SIZE);assert(fwrite(c.gpr,1,sizeof c.gpr,f)==sizeof c.gpr);uint32_t meta[4]={c.pc,c.status,c.stop_code,c.host_depth};assert(fwrite(meta,1,16,f)==16);fclose(f);char path[1024];snprintf(path,sizeof path,"%s.context.bin",argv[3]);f=fopen(path,"wb");assert(f);assert(fwrite(&c,1,sizeof c,f)==sizeof c);fclose(f);printf("ls_pointer_offset=%zu context_bytes=%zu\n",offsetof(spu_context,ls),sizeof c);return 0;}
'''
 out.mkdir(parents=True,exist_ok=True);(out/'oracle.c').write_text(prefix+helper+f+suffix)
 report={'production_hook_installed':True,'compiled_or_executed':False,'source_sha256':hashlib.sha256(source.encode()).hexdigest(),'original_function_lines':len(f.splitlines()),'direct_callee':'88E0 writes descriptor+50, retainsrealGPR/LSeffects','external_DMA_or_channels_in_selected_source':False,'input_required':'ActualC5B0entry fullLS262144+GPR2048, metadataimage10003/pcC5B0/guestlink/hostdepth/jobdescriptor/thread/epoch. Jobpreentryprobe isnotvalidkernelinput.','oracle_limits':'Logicalfunctioncallwithhostdepth1; nativeCPUoriginalnotSPUinterpreterdifferential; unexpectedtrampoline/channel/STOPabort. Originalfloathelpersretained. Codehashandrealentrycapturerequiredbeforeexecution.','capture_contract':'Approvedgeneratedentrywrapper(defaultOFF): cachedOFFbranch onceperC5B0call; armviauniquefileaftercombat; onceperactualworker(max4) snapshotsownedLS/GPR+metadata onlybeforebody, codeguardoriginalC5B0..D430+88E0..88F8; no substituteinput, no guestPC mutation. Dumpduringsafeownedexecution; diagnosticnotFPSrun.','SMC_reuse_limit':'Existingspu_channels.c2120fallbackcapturefilters6080<=pc<7000andSMCinterpreterfallback. C5B0isstaticnativeentryoutsidewindow, soexistingSPU_SMC_CAPTURE_PC cannotcaptureit. Reusefilelayout/hash/writehelpersconcept, notmisrepresentexistingflagworking.','recent_ring_consumer':'spu_recent_pcs usedbyspurs_policy.c398postmortem; existingrecentPCringnotcurrentlycontrolledbytraceOFF. Skippingisdiagnosticbehaviorchange andneeds explicitoptin/configcontract, notassumeunused.'}
 (out/'plan.json').write_text(json.dumps(report,indent=2));return report
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,default=R/'native/analysis/geom-c5b0-oracle');a=p.parse_args();print(json.dumps(prepare(a.output),indent=2))
