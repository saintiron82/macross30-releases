"""Preserve opt-in boundary profiling when regenerating guarded plugins."""
import pathlib,re
R=pathlib.Path(__file__).resolve().parents[2];p=R/'native/macross30/spu_posttask_native/guard.c';s=p.read_text()
if '#include "boundary_perf.h"' not in s:s=s.replace('static INIT_ONCE init_once=', '#include "boundary_perf.h"\nstatic INIT_ONCE init_once=')
for family in [0,1,2,4,6,8,9,10,11]:
 if f'macross_posttask_perf_begin(ctx,pc,{family})' in s:continue
 pattern=rf'  (macross_posttask_f{family}_spu_func_[0-9A-F]+\(ctx\);)'
 s=re.sub(pattern,lambda m:f'  int perf_token=macross_posttask_perf_begin(ctx,pc,{family});\n  '+m[1]+'\n  macross_posttask_perf_end(perf_token);',s)
p.write_text(s)
