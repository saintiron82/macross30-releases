"""Add the seventh proven original raw posttask plugin without changing older families."""
import pathlib,json
R=pathlib.Path(__file__).resolve().parents[2];A=R/'native/analysis/posttask-miss-6264';D=R/'native/macross30/spu_posttask_native';S=A/'prototype/capture0'
p=next(A.glob('smc_00*.json'));meta=json.loads(p.read_text());data=p.with_name(p.stem+'_ls.bin').read_bytes();code=data[0x6230:0x6e20]
with open(R/'analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf','rb') as f:f.seek(0xA571B0);assert f.read(len(code))==code
h=(S/'spu_recomp.h').read_text().replace('wide_','macross_posttask_f9_').replace('"spu_context.h"','"../../../vendor/ps3recomp-master/runtime/spu/spu_context.h"')
c=(S/'spu_recomp.c').read_text().split('/* Function table */')[0].replace('wide_','macross_posttask_f9_').replace('"spu_recomp.h"','"family9.h"').replace('"spu_helpers.h"','"../../../vendor/ps3recomp-master/runtime/spu/spu_helpers.h"')
(D/'family9.h').write_text(h);(D/'family9.c').write_text('/* Original Macross posttask raw plugin: EBOOT.elf0xA571B0, LS0x6230..0x6e20. Existing lifter output, actual captured-state full differential validated. */\n'+c)
p=D/'guard.c';g=p.read_text()
if '#include "family9.h"' not in g:
 decl='#include "family9.h"\nstatic const unsigned char expected9[]={'+','.join(str(x) for x in code)+'};\n'
 g=g.replace('static INIT_ONCE init_once=',decl+'static INIT_ONCE init_once=').replace('hits[6],misses','hits[7],misses')
 branch=''' if(pc==0x6264u&&!memcmp(ctx->ls+0x6230,expected9,sizeof(expected9))){
  LONG n=InterlockedIncrement(&hits[6]);if(n<=2)fprintf(stderr,"[posttask-native] family=9 pc=%05X exact_bytes=%zu hit=%ld\\n",pc,sizeof(expected9),n);
  ctx->pc=pc;ctx->status=SPU_STATUS_RUNNING;
  macross_posttask_f9_spu_func_00006264(ctx);
  return 1;
 }
'''
 g=g.replace(' LONG n=InterlockedIncrement(&misses);',branch+' LONG n=InterlockedIncrement(&misses);');p.write_text(g)
print('family9 generated, exact original3056bytes verified; guarded PC06264 only')

import runpy
runpy.run_path(str(R/'tools/build/enable_posttask_perf.py'), run_name='__main__')
