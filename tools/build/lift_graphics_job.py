"""Lift the next observed raw SPURS job; retain its full fingerprint/data."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
root=Path(__file__).resolve().parents[2]
sdk=root/'vendor/ps3recomp-master'
sys.path.insert(0,str(sdk/'tools'))
from spu_disasm import disassemble_spu
binary=root/'native/macross30/raw_jobs/job_0ae6346f73bc04a7.bin'
data=binary.read_bytes()
fp=1469598103934665603
for b in data: fp=((fp^b)*1099511628211)&0xffffffffffffffff
assert fp==0x0AE6346F73BC04A7
code_end=0x6F24
assert data[code_end-4:code_end]==bytes.fromhex('35000000')
seeds=set(range(0,code_end,4))
# This job branches indirectly into instruction interiors. Register every
# instruction in the verified code span so unchanged instructions execute
# compiled code rather than the SDK's missing-target interpreter fallback.
for ins in disassemble_spu(data[:code_end],0):
    if ins.mnemonic in ('br','bra','brsl','brasl','brz','brnz','brhz','brhnz'):
        for token in re.findall(r'0x[0-9A-Fa-f]+',ins.operands):
            t=int(token,16)
            if 0<=t<code_end and t%4==0: seeds.add(t)
out=root/'native/macross30/spu_graphics'
out.mkdir(exist_ok=True)
ordered=sorted(seeds)+[code_end]
(out/'functions.json').write_text(json.dumps([dict(start=hex(a),end=hex(b)) for a,b in zip(ordered,ordered[1:])]),encoding='utf-8')
subprocess.run([sys.executable,'-X','utf8',str(sdk/'tools/spu_lifter.py'),str(binary),
    '--base','0','--functions',str(out/'functions.json'),'--return-entries',
    '--code-end',hex(code_end),'--symbol-prefix','macross_graphics_',
    '--output',str(out),'--report',str(out/'report.json')],check=True)
report=json.loads((out/'report.json').read_text())
assert not report['unsupported'],report['unsupported']
for name in ('spu_recomp.c','spu_recomp.h'):
    p=out/name
    p.write_text(p.read_text().replace('"../../runtime/spu/','"'),encoding='utf-8')
(out/'source.json').write_text(json.dumps(dict(size=len(data),fingerprint=hex(fp),
    sha256=hashlib.sha256(data).hexdigest(),code_end=hex(code_end)),indent=2),encoding='utf-8')
