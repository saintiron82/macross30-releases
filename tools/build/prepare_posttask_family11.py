"""Add the ninth proven original raw posttask plugin without changing older families."""
import pathlib,json
R=pathlib.Path(__file__).resolve().parents[2];A=R/'native/analysis/posttask-miss-6914';D=R/'native/macross30/spu_posttask_native';S=A/'prototype/capture0'
p=next(A.glob('smc_00*.json'));meta=json.loads(p.read_text());data=p.with_name(p.stem+'_ls.bin').read_bytes();code=data[0x6230:0x6ad0]
with open(R/'analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf','rb') as f:f.seek(0xA58130);assert f.read(len(code))==code
h=(S/'spu_recomp.h').read_text().replace('wide_','macross_posttask_f11_').replace('"spu_context.h"','"../../../vendor/ps3recomp-master/runtime/spu/spu_context.h"')
c=(S/'spu_recomp.c').read_text().split('/* Function table */')[0].replace('wide_','macross_posttask_f11_').replace('"spu_recomp.h"','"family11.h"').replace('"spu_helpers.h"','"../../../vendor/ps3recomp-master/runtime/spu/spu_helpers.h"')
(D/'family11.h').write_text(h);(D/'family11.c').write_text('/* Original Macross posttask raw plugin: EBOOT.elf0xA58130, LS0x6230..0x6ad0. Existing lifter output, actual captured-state full differential validated. */\n'+c)
p=D/'guard.c';g=p.read_text()
if '#include "family11.h"' not in g:
 decl='#include "family11.h"\nstatic const unsigned char expected11[]={'+','.join(str(x) for x in code)+'};\n'
 g=g.replace('static INIT_ONCE init_once=',decl+'static INIT_ONCE init_once=').replace('hits[8],misses','hits[9],misses')
 branch=''' if(pc==0x6914u&&!memcmp(ctx->ls+0x6230,expected11,sizeof(expected11))){
  LONG n=InterlockedIncrement(&hits[8]);if(n<=2)fprintf(stderr,"[posttask-native] family=11 pc=%05X exact_bytes=%zu hit=%ld\\n",pc,sizeof(expected11),n);
  ctx->pc=pc;ctx->status=SPU_STATUS_RUNNING;
  macross_posttask_f11_spu_func_00006914(ctx);
  return 1;
 }
'''
 g=g.replace(' LONG n=InterlockedIncrement(&misses);',branch+' LONG n=InterlockedIncrement(&misses);');p.write_text(g)
print('family11 generated, exact original2208bytes verified; guarded PC06914 only')

import runpy
runpy.run_path(str(R/'tools/build/enable_posttask_perf.py'), run_name='__main__')
