"""Reproduce the observed raw SPURS job lift and validate all emitted opcodes."""
import json
from pathlib import Path
import re
import subprocess
import sys

root=Path(__file__).resolve().parents[2]
subprocess.run([sys.executable,str(root/'tools/build/extract_native_job.py')],check=True)
sdk=root/'vendor/ps3recomp-master'
sys.path.insert(0,str(sdk/'tools'))
from spu_disasm import disassemble_spu
binary=root/'native/macross30/raw_jobs/job_4c091e2c0bd5c570.bin'
data=binary.read_bytes()
seeds={0}
for ins in disassemble_spu(data,0):
    if ins.mnemonic in ('br','bra','brsl','brasl','brz','brnz','brhz','brhnz'):
        for token in re.findall(r'0x[0-9A-Fa-f]+',ins.operands):
            target=int(token,16)
            if 0<=target<len(data) and target%4==0: seeds.add(target)
out=root/'native/macross30/spu_notify'
out.mkdir(exist_ok=True)
ordered=sorted(seeds)+[len(data)]
functions=out/'functions.json'
functions.write_text(json.dumps([dict(start=hex(a),end=hex(b)) for a,b in zip(ordered,ordered[1:])]),encoding='utf-8')
subprocess.run([sys.executable,'-X','utf8',str(sdk/'tools/spu_lifter.py'),str(binary),
    '--base','0','--functions',str(functions),'--return-entries',
    '--symbol-prefix','macross_notify_','--output',str(out),'--report',str(out/'report.json')],check=True)
report=json.loads((out/'report.json').read_text())
assert not report['unsupported'],report['unsupported']
for name in ('spu_recomp.c','spu_recomp.h'):
    path=out/name
    path.write_text(path.read_text().replace('"../../runtime/spu/','"'),encoding='utf-8')
print('Raw job generated; unsupported opcodes:',report['unsupported'])
