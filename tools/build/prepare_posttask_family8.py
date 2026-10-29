"""Add the sixth proven original raw posttask plugin without changing older families."""
import pathlib,json
R=pathlib.Path(__file__).resolve().parents[2];A=R/'native/analysis/posttask-miss-6724';D=R/'native/macross30/spu_posttask_native';S=A/'prototype/capture0'
p=next(A.glob('smc_00*.json'));meta=json.loads(p.read_text());data=p.with_name(p.stem+'_ls.bin').read_bytes();code=data[0x6230:0x6900]
with open(R/'analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf','rb') as f:f.seek(0xA55C30);assert f.read(len(code))==code
h=(S/'spu_recomp.h').read_text().replace('wide_','macross_posttask_f8_').replace('"spu_context.h"','"../../../vendor/ps3recomp-master/runtime/spu/spu_context.h"')
c=(S/'spu_recomp.c').read_text().split('/* Function table */')[0].replace('wide_','macross_posttask_f8_').replace('"spu_recomp.h"','"family8.h"').replace('"spu_helpers.h"','"../../../vendor/ps3recomp-master/runtime/spu/spu_helpers.h"')
(D/'family8.h').write_text(h);(D/'family8.c').write_text('/* Original Macross posttask raw plugin: EBOOT.elf0xA55C30, LS0x6230..0x6900. Existing lifter output, actual three-state full differential validated. */\n'+c)
p=D/'guard.c';g=p.read_text()
if '#include "family8.h"' not in g:
 decl='#include "family8.h"\nstatic const unsigned char expected8[]={'+','.join(str(x) for x in code)+'};\n'
 g=g.replace('static INIT_ONCE init_once=',decl+'static INIT_ONCE init_once=').replace('hits[5],misses','hits[6],misses')
 branch=''' if(pc==0x6724u&&!memcmp(ctx->ls+0x6230,expected8,sizeof(expected8))){
  LONG n=InterlockedIncrement(&hits[5]);if(n<=2)fprintf(stderr,"[posttask-native] family=8 pc=%05X exact_bytes=%zu hit=%ld\\n",pc,sizeof(expected8),n);
  ctx->pc=pc;ctx->status=SPU_STATUS_RUNNING;
  macross_posttask_f8_spu_func_00006724(ctx);
  return 1;
 }
'''
 g=g.replace(' LONG n=InterlockedIncrement(&misses);',branch+' LONG n=InterlockedIncrement(&misses);');p.write_text(g)
print('family8 generated, exact original1744bytes verified; guarded PC06724 only')

import runpy
runpy.run_path(str(R/'tools/build/enable_posttask_perf.py'), run_name='__main__')
