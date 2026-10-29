"""Add the eighth proven original raw posttask plugin without changing older families."""
import pathlib,json
R=pathlib.Path(__file__).resolve().parents[2];A=R/'native/analysis/posttask-miss-631c';D=R/'native/macross30/spu_posttask_native';S=A/'prototype/capture0'
p=next(A.glob('smc_00*.json'));meta=json.loads(p.read_text());data=p.with_name(p.stem+'_ls.bin').read_bytes();code=data[0x6230:0x64d0]
with open(R/'analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf','rb') as f:f.seek(0xA58D30);assert f.read(len(code))==code
h=(S/'spu_recomp.h').read_text().replace('wide_','macross_posttask_f10_').replace('"spu_context.h"','"../../../vendor/ps3recomp-master/runtime/spu/spu_context.h"')
c=(S/'spu_recomp.c').read_text().split('/* Function table */')[0].replace('wide_','macross_posttask_f10_').replace('"spu_recomp.h"','"family10.h"').replace('"spu_helpers.h"','"../../../vendor/ps3recomp-master/runtime/spu/spu_helpers.h"')
(D/'family10.h').write_text(h);(D/'family10.c').write_text('/* Original Macross posttask raw plugin: EBOOT.elf0xA58D30, LS0x6230..0x64d0. Existing lifter output, actual captured-state full differential validated. */\n'+c)
p=D/'guard.c';g=p.read_text()
if '#include "family10.h"' not in g:
 decl='#include "family10.h"\nstatic const unsigned char expected10[]={'+','.join(str(x) for x in code)+'};\n'
 g=g.replace('static INIT_ONCE init_once=',decl+'static INIT_ONCE init_once=').replace('hits[7],misses','hits[8],misses')
 branch=''' if(pc==0x631Cu&&!memcmp(ctx->ls+0x6230,expected10,sizeof(expected10))){
  LONG n=InterlockedIncrement(&hits[7]);if(n<=2)fprintf(stderr,"[posttask-native] family=10 pc=%05X exact_bytes=%zu hit=%ld\\n",pc,sizeof(expected10),n);
  ctx->pc=pc;ctx->status=SPU_STATUS_RUNNING;
  macross_posttask_f10_spu_func_0000631C(ctx);
  return 1;
 }
'''
 g=g.replace(' LONG n=InterlockedIncrement(&misses);',branch+' LONG n=InterlockedIncrement(&misses);');p.write_text(g)
print('family10 generated, exact original672bytes verified; guarded PC0631C only')

import runpy
runpy.run_path(str(R/'tools/build/enable_posttask_perf.py'), run_name='__main__')
