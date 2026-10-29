"""Build an isolated local-dispatch candidate for geom2's 0x13250 kernel.

The normal merged lift still exits through a computed r127 branch into its
one-instruction interior entries. Keep that transfer inside the same body
when interrupts are disabled and the target is an aligned local instruction.
All other cases retain the original dispatcher. No guest bytes are changed.
"""
import hashlib
import argparse
from pathlib import Path
import re
import sys

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'vendor/ps3recomp-master/tools'))
from spu_disasm import disassemble_spu
from spu_lifter import SPULifter

START,END=0x13250,0x13e78
TARGETS=(0x1395c,0x139d4,0x13ad8)
# End is the already-merged function's existing extent, not a new grouping.
BASE_SHA='f2693f33fee37485b313046c016f818094ccd626268eb099733ac4addecad433'
BIN_SHA='2385322fb917c4c937449ee226ba60f5ef796b7a97a5552b1ae8d2aca99f25e3'
VERIFIED_SHA='819b73fbce196edf00f500dd5fb89f015019a7b55536d4fdabdaf932e2cf1178'


class LocalLifter(SPULifter):
    def _translate(self,insn,func):
        statement=super()._translate(insn,func)
        if 'ctx->pc = ctx->gpr[127]._u32[0]; g_spu_trampoline_fn = spu_indirect_branch; return;' == statement:
            cases=' '.join(f'case 0x{pc:X}: LOCAL_DISPATCH_TEST_HIT(); goto local_{pc:08X};' for pc in TARGETS)
            statement=('ctx->pc = ctx->gpr[127]._u32[0]; '
                       'if (!ctx->int_enable && ctx->image_id == 10005 && !ctx->policy_mode && '
                       '!ctx->resident_ovl && !ctx->resident_task && '
                       '!ctx->resident_code[0].image_id && !ctx->resident_code[1].image_id && '
                       '!ctx->resident_code[2].image_id && !ctx->resident_code[3].image_id) '
                       '{ switch (ctx->pc) { '+cases+' default: break; } } '
                       'g_spu_trampoline_fn = spu_indirect_branch; return;')
        return (f'local_{insn.addr:08X}:;\n    ' if insn.addr in TARGETS else '')+statement


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apply',action='store_true',help='Install the hash-pinned, offline-validated candidate')
    args=parser.parse_args()
    base=ROOT/'native/macross30/spu_gameplay/geom2/spu_recomp.c'
    raw=base.read_bytes()
    if hashlib.sha256(raw).hexdigest()==VERIFIED_SHA:
        print('geom2 local dispatch already applied')
        return
    assert hashlib.sha256(raw).hexdigest()==BASE_SHA,'Review source changes before rebuilding candidate'
    binary=(ROOT/'native/macross30/raw_jobs/job_1d015e84b1917b2e.bin').read_bytes()
    assert hashlib.sha256(binary).hexdigest()==BIN_SHA
    insns=disassemble_spu(binary[:0x149c8],0)
    # Recover the original classifications from the already emitted body by
    # requiring a fresh baseline lift to match it byte for byte below.
    source=raw.decode('utf-8').replace('\r\n','\n')
    signature='void macross_geom2_spu_func_00013250(spu_context* ctx) {'
    begin=source.index(signature)
    finish=source.index('\n}\n',begin)+2
    old=source[begin:finish]
    def render(lifter):
        lifter.func_starts=set(range(0,0x149c8,4))
        func=lifter.lift_function(insns,START,END)
        return signature+'\n'+'\n'.join('    '+line if not line.startswith('loc_') else line for line in func.body_lines)+'\n}'
    plain=render(SPULifter(prefix='macross_geom2_'))
    assert plain==old,'Kernel extent or return classifications differ from frozen original'
    updated=render(LocalLifter(prefix='macross_geom2_'))
    assert updated.count('switch (ctx->pc)')==1
    folder=ROOT/'native/analysis/geom2-local-dispatch-candidate'
    folder.mkdir(exist_ok=True)
    backup=folder/'baseline-spu_recomp.c'
    if backup.exists():assert backup.read_bytes()==raw,'Baseline backup differs'
    else:backup.write_bytes(raw)
    instrumentation=('\n#ifdef MACROSS_GEOM2_LOCAL_DISPATCH_TEST\n'
                     'unsigned long long g_geom2_local_dispatch_taken;\n'
                     '#define LOCAL_DISPATCH_TEST_HIT() (++g_geom2_local_dispatch_taken)\n'
                     '#else\n#define LOCAL_DISPATCH_TEST_HIT() ((void)0)\n#endif\n')
    (folder/'spu_recomp.c').write_text(source[:begin]+instrumentation+updated+source[finish:],encoding='utf-8')
    (folder/'spu_recomp.h').write_bytes((base.parent/'spu_recomp.h').read_bytes())
    candidate=(folder/'spu_recomp.c').read_bytes()
    assert hashlib.sha256(candidate).hexdigest()==VERIFIED_SHA,'Candidate changed; repeat offline validation before applying'
    if args.apply:
        base.write_bytes(candidate)
        print('Applied verified geom2 local dispatch; original retained in '+str(backup))
    else:
        print('Candidate only: 0x13250 local computed branch; original function body verified before transformation')


if __name__=='__main__':main()
