"""Lift reviewed captured gameplay SPURS jobs without changing guest bytes.

The end of each reviewed code span is a final compiler helper's backward branch
plus alignment lnop. Eight zero alignment bytes precede the original constant
tail (assertion strings for geometry, masks/constants for animation). The full
tail is retained by the runtime loader, never emitted as instructions.
Exhaustive aligned entries preserve SPURS interior resumptions.
The JOBCRT Ver14 image has separate bootstrap and executable spans around its
original header; only those reviewed code islands receive native entries.
"""
import hashlib
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
SDK = ROOT / 'vendor/ps3recomp-master'
sys.path.insert(0, str(SDK / 'tools'))
from find_spu_functions import detect_functions
from spu_disasm import disassemble_spu
from spu_lifter import compute_bi_r0_jumps

JOBS = [
    dict(fingerprint='3b9f76869d920608', size=89984,
         sha256='9738c671984831af4ea6765bbc70583e00f5aca01313b96193bc74f04c6399e0',
         code_end=0x14B48, strings_start=0x14B50,
         terminal_branch=0x14B40, branch_target=0x142E8,
         source_component='SpursJobEdgeGeom', label='geom1', image_id=10003),
    dict(fingerprint='c63600e59cd05d37', size=91392,
         sha256='8b5e0c38ee2b556b4029adb0834b696ffba344883b2da33f7c04164398fdd38d',
         code_end=0x150B8, strings_start=0x150C0,
         terminal_branch=0x150B0, branch_target=0x14858,
         source_component='SpursJobEdgeGeom3', label='geom3', image_id=10004),
    dict(fingerprint='1d015e84b1917b2e', size=89600,
         sha256='2385322fb917c4c937449ee226ba60f5ef796b7a97a5552b1ae8d2aca99f25e3',
         code_end=0x149C8, strings_start=0x149D0,
         terminal_branch=0x149C0, branch_target=0x14168,
         source_component='SpursJobEdgeGeom2', label='geom2', image_id=10005),
    dict(fingerprint='aeb25f55b679f161', size=54096,
         sha256='007a10b264aa1a9b4b9a8336a96373776ba2941e647be92d5bb26b04bead352a',
         code_end=0xC878, data_start=0xC880, strings_start=0xC8C0,
         terminal_branch=0xC870, branch_target=0xC79C,
         tail_prefix='0000000000007fff0000000000000000',
         string_prefix=b'( %f %f %f %f )\n\0',
         source_component='edgeAnim', label='anim', image_id=10006),
    dict(fingerprint='b8c682fceff9d543', size=14800,
         sha256='6e9519b4747256ab01e1b99576585a6dbf4b9730bc6719cd3c716000f1c79e79',
         code_end=0x35B8, strings_start=0x35C0,
         code_ranges=[(0, 0x20), (0x60, 0x35B8)],
         terminal_branch=0x35B4, terminal_mnemonic='bi', terminal_operand='$r0',
         final_mnemonic='bi', string_prefix=b'\n_exit() has been called!!',
         original_eboot_offset=0x9EA800,
         source_component='JOBCRT Ver', label='jobcrt14', image_id=10007),
    dict(fingerprint='84665e22d700ddb1', size=9680,
         sha256='0eca66d2d68011a320e2a23402c698f5eb6cf9f8dd03caa0bf3bf9b07c5a325f',
         code_end=0x2258, strings_start=0x2260,
         code_ranges=[(0, 0x20), (0x60, 0x2258)],
         terminal_branch=0x2254, terminal_mnemonic='bi', terminal_operand='$r0',
         final_mnemonic='bi', string_prefix=b'\n_exit() has been called!!',
         original_eboot_offset=0x9A2E80,
         source_component='JOBCRT Ver', label='jobcrt14_b', image_id=10008),
    dict(fingerprint='039ea2388c0b6fcf', size=9104,
         sha256='870597311935a69ded11cb697d9210d4fb60831746f3ed0598c6cabac335e4d7',
         code_end=0x1D70, strings_start=0x1D70, tail_padding_bytes=0,
         code_ranges=[(0, 0x20), (0x60, 0x1D70)],
         terminal_branch=0x1D6C, terminal_mnemonic='bi', terminal_operand='$r0',
         final_mnemonic='bi', string_prefix=b'\n_exit() has been called!!',
         original_eboot_offset=0x9D0E80, component_range=(0x30, 0x3C),
         source_component='JOBCRT Ver14', label='jobcrt14_c', image_id=10009),
    dict(fingerprint='ee4da7a6ab0b3832', size=9296,
         sha256='918869ed9fa83bfb96d592c75fa606982ffb61925bdc3fda5d9f66feb1de4261',
         code_end=0x1FC8, strings_start=0x1FD0,
         code_ranges=[(0, 0x20), (0x60, 0x1FC8)],
         terminal_branch=0x1FC4, terminal_mnemonic='bi', terminal_operand='$r0',
         final_mnemonic='bi', string_prefix=b'\n_exit() has been called!!',
         original_eboot_offset=0xA3B880, component_range=(0x30, 0x3C),
         source_component='JOBCRT Ver14', label='jobcrt14_d', image_id=10010),
    dict(fingerprint='d72ffdaeb98c90ab', size=95616,
         sha256='f0511b249d2a51ae9e7f1deaf896a148e9d9d0f103d71286f29f841c76ea316d',
         code_end=0x16138, strings_start=0x16140,
         terminal_branch=0x16130, branch_target=0x158D8,
         original_eboot_offset=0x9D3280,
         source_component='SpursJobEdgeGeom6', label='geom6', image_id=10011),
    dict(fingerprint='218a58efdd0bc6c6', size=92288,
         sha256='fa5fea6c8a56f1a697dd127dc62714f6413731a771a0f20551ddaef735bc7fa5',
         code_end=0x153F8, strings_start=0x15400,
         terminal_branch=0x153F0, branch_target=0x14B98,
         original_eboot_offset=0x9EE200,
         source_component='SpursJobEdgeGeom7', label='geom7', image_id=10012),
    dict(fingerprint='79325615aa2c3534', size=87296,
         sha256='738a583addcbc4ce66e4a24fe0dcc9ea70e90a8e720abb0b223a9e99d2ef1441',
         code_end=0x140B8, strings_start=0x140C0,
         terminal_branch=0x140B0, branch_target=0x13858,
         original_eboot_offset=0x9BB980,
         source_component='SpursJobEdgeGeom4', label='geom4', image_id=10013),
    dict(fingerprint='6ae2268e5cb83a48', size=89216,
         sha256='35b633c6c77bbe8f91bb957635081553dc82792c57140383be9a1e25ee5e6690',
         code_end=0x14828, strings_start=0x14830,
         terminal_branch=0x14820, branch_target=0x13FC8,
         original_eboot_offset=0x96EA00,
         source_component='SpursJobEdgeGeom5', label='geom5', image_id=10014),
]


def fingerprint(data):
    # Exact SDK spu_workload_fingerprint offset basis, not standard FNV-1a's.
    value = 1469598103934665603
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


def insn_record(insn):
    return dict(address=hex(insn.addr), word=f'{insn.raw:08x}',
                mnemonic=insn.mnemonic, operands=insn.operands)


def lift(job, output, merge_chunks=False, max_group_insns=4000):
    binary = ROOT / 'native/macross30/raw_jobs' / f'job_{job["fingerprint"]}.bin'
    data = binary.read_bytes()
    assert len(data) == job['size']
    assert hashlib.sha256(data).hexdigest() == job['sha256']
    assert fingerprint(data) == int(job['fingerprint'], 16)
    if 'original_eboot_offset' in job:
        with (ROOT / 'analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf').open('rb') as original:
            original.seek(job['original_eboot_offset'])
            assert original.read(len(data)) == data, 'Captured job differs from original EBOOT bytes'
    end, strings = job['code_end'], job['strings_start']
    tail_start = job.get('data_start', strings)
    assert data[end:tail_start] == bytes(job.get('tail_padding_bytes', 8))
    if 'tail_prefix' in job:
        assert data[tail_start:tail_start + 16] == bytes.fromhex(job['tail_prefix'])
    assert data[strings:].startswith(job.get('string_prefix', b'%s:%d: %s -- assertion failed.\n\0'))
    component_start, component_end = job.get('component_range', (strings, strings + 512))
    assert job['source_component'].encode() in data[component_start:component_end]
    ranges = job.get('code_ranges', [(0, end)])
    entries = [a for start, stop in ranges for a in range(start, stop, 4)]
    insns = [insn for start, stop in ranges
             for insn in disassemble_spu(data[start:stop], start)]
    by_addr = {insn.addr: insn for insn in insns}
    assert [insn.addr for insn in insns] == entries
    assert not [insn for insn in insns if insn.mnemonic == '.word']
    branch = by_addr[job['terminal_branch']]
    if 'terminal_mnemonic' in job:
        assert branch.mnemonic == job['terminal_mnemonic']
        assert branch.operands == job['terminal_operand']
    else:
        assert branch.mnemonic == 'br' and int(branch.operands, 0) == job['branch_target']
    assert by_addr[end - 4].mnemonic == job.get('final_mnemonic', 'lnop')
    # Recover original broader bounds BEFORE splitting into one-instruction
    # entries. Otherwise the r0 producer context would disappear and could
    # turn a computed tail branch into a host return. Preserve classifications
    # explicitly through --force-indirect, as the ELF SPU wrapper does.
    prior, _ = detect_functions(data[:end], base_override=0, verbose=False, raw=True)
    jumps = compute_bi_r0_jumps(insns, prior)
    coarse_jumps = compute_bi_r0_jumps(insns, [(0, end)])
    assert jumps == coarse_jumps, 'Review differing computed-r0 classifications before lifting'
    direct = []
    for insn in insns:
        if insn.mnemonic in ('br', 'bra', 'brsl', 'brasl', 'brz', 'brnz', 'brhz', 'brhnz'):
            target = int(re.findall(r'0x[0-9A-Fa-f]+', insn.operands)[-1], 16)
            direct.append((insn.addr, target))
    outside = [(hex(addr), hex(target)) for addr, target in direct
               if target not in set(entries)]
    assert not outside, f'Direct branch outside reviewed code span: {outside}'
    output.mkdir(parents=True, exist_ok=True)
    functions = output / 'functions.json'
    functions.write_text(json.dumps([dict(start=hex(a), end=hex(a + 4))
                                     for a in entries]), encoding='utf-8')
    (output / 'original_bounds.json').write_text(
        json.dumps([dict(start=hex(a), end=hex(b)) for a, b in prior]), encoding='utf-8')
    prefix = f'macross_{job["label"]}_'
    command = [sys.executable, '-X', 'utf8', str(SDK / 'tools/spu_lifter.py'), str(binary),
               '--base', '0', '--functions', str(functions), '--return-entries',
               '--code-end', hex(end), '--symbol-prefix', prefix,
               '--output', str(output), '--report', str(output / 'report.json')]
    if jumps:
        command.extend(['--force-indirect', ','.join(hex(a) for a in sorted(jumps))])
    if merge_chunks:
        command.extend(['--merge-chunks', '--max-group-insns', str(max_group_insns)])
    subprocess.run(command, check=True)
    report = json.loads((output / 'report.json').read_text(encoding='utf-8'))
    assert not report['unsupported'], report['unsupported']
    for name in ('spu_recomp.c', 'spu_recomp.h'):
        path = output / name
        path.write_text(path.read_text(encoding='utf-8').replace('"../../runtime/spu/', '"'),
                        encoding='utf-8')
    emitted = (output / 'spu_recomp.c').read_text(encoding='utf-8')
    table = emitted[emitted.index('static const spu_func_entry'):]
    registered = [int(address, 16) for address in re.findall(r'\{ 0x([0-9A-Fa-f]+)u,', table)]
    assert registered == entries, 'Generated registration table lost static entries'
    assert binary.read_bytes() == data, 'Guest source changed during lift'
    audit = dict(source=str(binary.relative_to(ROOT)), size=len(data), sha256=job['sha256'],
                 sdk_fingerprint='0x' + job['fingerprint'], runtime_image_id=job['image_id'],
                 source_component=job['source_component'], code_base='0x0', code_end=hex(end),
                 assertion_strings_start=hex(strings) if job['label'] not in ('anim', 'jobcrt14', 'jobcrt14_b', 'jobcrt14_c', 'jobcrt14_d') else None,
                 constant_tail_start=hex(tail_start), strings_start=hex(strings),
                 terminal_branch=insn_record(branch),
                 terminal_alignment=insn_record(by_addr[end - 4]) if by_addr[end - 4].mnemonic == 'lnop' else None,
                 final_instruction=insn_record(by_addr[end - 4]),
                 original_eboot_offset=hex(job['original_eboot_offset']) if 'original_eboot_offset' in job else None,
                 original_tail_retained=True, guest_bytes_modified=False,
                 reviewed_code_ranges=[[hex(a), hex(b)] for a, b in ranges],
                 recovered_function_count=len(prior), static_entry_count=len(entries), entry_stride=4,
                 generated_registration_entries_verified=len(registered),
                 preserved_computed_r0_jump_sites=[hex(a) for a in sorted(jumps)],
                 computed_r0_coarse_scan_agrees=True, outside_direct_branches=outside,
                 merge_chunks=merge_chunks,
                 max_group_insns=max_group_insns if merge_chunks else None,
                 unsupported_decode=[], unsupported_lift=report['unsupported'],
                 first_instructions=[insn_record(i) for i in insns[:12]],
                 final_instructions=[insn_record(i) for i in insns[-16:]],
                 tail_ascii=[dict(offset=hex(strings + match.start()), text=match.group().decode('ascii'))
                             for match in list(re.finditer(rb'[ -~]{12,}', data[strings:]))[:8]],
                 generated_c_sha256=hashlib.sha256((output / 'spu_recomp.c').read_bytes()).hexdigest(),
                 semantic_equivalence_verified=False, runtime_coverage_verified=False,
                 scope='Static instruction generation only; no synthetic job completion or guest data edits')
    (ROOT / 'native/analysis' / f'gameplay_job_audit_{job["label"]}.json').write_text(
        json.dumps(audit, indent=2), encoding='utf-8')
    return audit


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--only', choices=[job['label'] for job in JOBS],
                        help='Regenerate only this image; preserve other generated jobs and audits')
    parser.add_argument('--merge-chunks', action='store_true',
                        help='Use SDK merged bodies while retaining every original registered entry')
    parser.add_argument('--max-group-insns', type=int, default=4000,
                        help='Maximum merged-body size; SDK default is 4000 instructions')
    args = parser.parse_args()
    if not 1 <= args.max_group_insns <= 4000:
        parser.error('--max-group-insns must be in 1..4000')
    output = ROOT / 'native/macross30/spu_gameplay'
    audits = []
    for job in JOBS:
        audit_path = ROOT / 'native/analysis' / f'gameplay_job_audit_{job["label"]}.json'
        if args.only and args.only != job['label'] and audit_path.exists():
            audits.append(json.loads(audit_path.read_text(encoding='utf-8')))
        else:
            audits.append(lift(job, output / job['label'], args.merge_chunks, args.max_group_insns))
    lines = ['#include "spu_workload.h"', '#include "spu_context.h"',
             'extern void spu_begin_image(int);']
    for job in JOBS:
        prefix = 'macross_' + job['label'] + '_'
        lines.extend([f'extern void {prefix}spu_recomp_register(void);',
                      f'extern void {prefix}spu_func_00000000(spu_context*);'])
    lines.extend(['void macross_gameplay_register(void)', '{'])
    for job in JOBS:
        prefix = 'macross_' + job['label'] + '_'
        lines.extend([f'    spu_begin_image({job["image_id"]});',
                      f'    {prefix}spu_recomp_register();',
                      f'    spu_workload_register_img(0x{job["fingerprint"].upper()}ULL,',
                      f'        {prefix}spu_func_00000000,{job["image_id"]},'
                      f'"Macross {job["source_component"]} captured original job");'])
    lines.extend(['    spu_begin_image(0);', '}', ''])
    (output / 'register.c').write_text('\n'.join(lines), encoding='utf-8')
    if not (output / 'register.h').exists():
        (output / 'register.h').write_text(
            '#pragma once\n#ifdef __cplusplus\nextern "C" {\n#endif\n'
            'void macross_gameplay_register(void);\n#ifdef __cplusplus\n}\n#endif\n', encoding='utf-8')
    (ROOT / 'native/analysis/gameplay_job_audit.json').write_text(
        json.dumps(dict(jobs=audits, registration_api='macross_gameplay_register',
                        runtime_verified=False), indent=2), encoding='utf-8')
    print(json.dumps([dict(fingerprint=a['sdk_fingerprint'], entries=a['static_entry_count'],
                           code_end=a['code_end']) for a in audits], indent=2))


if __name__ == '__main__':
    main()
