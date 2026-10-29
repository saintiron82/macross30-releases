"""Macross 30 BLJS10184 SPU executable/constant boundaries.

The stripped executable PT_LOADs include constant tails. Each boundary below
follows the final compiler-generated restore-r0/r80/r81/r126, restore-r1,
bi-r0 epilogue. The tails begin with shuffle masks, zlib diagnostic strings,
or floating-point coefficient tables. We keep all ELF/data bytes unchanged;
only the addresses offered to the instruction lifter are bounded.
"""
import hashlib
import json
import os
import re
from pathlib import Path
import subprocess
import sys


BOUNDS = {
    'b00089288811e0386af134b05f015fe4dd39173f00250001bf158290adbf67b8': 0xb2a0,
    '2f512e1a72325554e198ffd3ae1e76a3b349caa24715ffdd8cf4b0873b32a3ff': 0x5fc8,
    '9d9d89391357928b257cb9f0fbee7733ca22968c798f292db13b36f308c82158': 0xad30,
    'c068f107cb21d183649f2d435ba197e9997753bfea23f94d0958716a99b6946f': 0x11f30,
}


def insert_queue_backoff(source, prefix, code, base, digest):
    """One reviewed ELF only; atomic-failure retry at A0E8 is untouched."""
    if digest != '9d9d89391357928b257cb9f0fbee7733ca22968c798f292db13b36f308c82158':
        return source, 0
    for pc, word in [(0xA134, 0x327F8100), (0xA140, 0x34024080),
                     (0xA0E8, 0x217F8A95)]:
        if code[pc-base:pc-base+4] != word.to_bytes(4, 'big'):
            raise ValueError(f'Queue backoff original instruction guard failed at {pc:X}')
    sequence = (
        '        ctx->gpr[38] = spu_ceqi(ctx->gpr[80], 0);\n'
        '        ctx->gpr[37] = spu_nor(ctx->gpr[38], ctx->gpr[38]);\n'
        '        ctx->gpr[20] = spu_sfi(ctx->gpr[37], 0);\n'
        f'        {{ ctx->pc = 0x9D3C; g_spu_trampoline_fn = {prefix}spu_func_00009D3C; return; }}')
    if source.count(sequence) != 1:
        raise ValueError('Queue backoff requires exactly one reviewed A128..A134 sequence')
    # Insert after r20 is computed, immediately before the original A134 BR.
    before, tail = sequence.rsplit('\n', 1)
    source = source.replace(sequence, before + '\n        macross_spu_queue_retry(ctx);\n' + tail)
    pattern = (r'(?m)^(void\s+' + re.escape(prefix) +
               r'spu_func_0000A140\(spu_context\* ctx\)\s*\{)')
    source, count = re.subn(pattern, r'\1\n    macross_spu_queue_activity(ctx);', source)
    if count != 1:
        raise ValueError('Queue backoff requires the exact A140 return entry once')
    source = ('#include "spu/spu_context.h"\n'
              'extern void macross_spu_queue_retry(spu_context*);\n'
              'extern void macross_spu_queue_activity(spu_context*);\n' + source)
    return source, 1
def insert_function_perf(source, prefix, digest):
    """Host-only sampled scopes; generated guest statements stay unchanged."""
    if digest not in (
        '2f512e1a72325554e198ffd3ae1e76a3b349caa24715ffdd8cf4b0873b32a3ff',
        'b00089288811e0386af134b05f015fe4dd39173f00250001bf158290adbf67b8',
    ):
        return source, 0
    # Cleanup scopes are incompatible with mandatory host tail calls. Current
    # emitter only defines SPU_TAILCALL; reject future actual uses explicitly.
    if re.search(r'(?m)^(?!\s*#).*\bSPU_TAILCALL\s*\(', source):
        raise ValueError('Function profiling cannot wrap mandatory tail calls')
    if 'MACROSS_SPU_PERF_SCOPE(ctx,' in source:
        raise ValueError('Function profiling scopes already inserted')
    marker = '/* Indirect-dispatch stubs for cross-function targets'
    body, separator, tail = source.partition(marker)
    pattern = (r'(?m)^(void\s+(?:__attribute__\(\(weak\)\)\s+)?' +
               re.escape(prefix) + r'spu_func_([0-9A-F]{8})'
               r'\(spu_context\* ctx\)\s*\{)')
    entries = set()
    def scoped(match):
        pc = int(match.group(2), 16)
        if pc in entries or pc >= BOUNDS[digest]:
            raise ValueError('Duplicate or out-of-bounds profiled SPU entry')
        entries.add(pc)
        return match.group(1) + f'\n    MACROSS_SPU_PERF_SCOPE(ctx, 0x{pc:X}u);'
    body, count = re.subn(pattern, scoped, body)
    if not count:
        raise ValueError('No actual lifted functions found for profiling')
    preamble = ('#ifdef MACROSS_SPU_FUNCTION_PROFILE\n'
                '#include "spu_function_perf.h"\n'
                '#else\n#define MACROSS_SPU_PERF_SCOPE(ctx, pc) ((void)0)\n#endif\n')
    return preamble + body + separator + tail, count


args = sys.argv[1:]
prefix = args[args.index('--symbol-prefix') + 1]
digest = prefix.removeprefix('spu_').removesuffix('_')
if digest not in BOUNDS:
    raise SystemExit('No reviewed code boundary for SPU image ' + digest)
image = Path(args[0]).parent / 'image.elf'
if hashlib.sha256(image.read_bytes()).hexdigest() != digest:
    raise SystemExit('SPU source image hash mismatch')
code = Path(args[0]).read_bytes()
base = int(args[args.index('--base') + 1], 0)
end = BOUNDS[digest]
if code[end - base - 4:end - base] != bytes.fromhex('35000000'):
    raise SystemExit('Expected terminal bi r0 not found')
print(f'[macross-spu] {digest[:12]} code end 0x{end:X}; constant tail retained in ELF', flush=True)
# Opt-in: SPURS resumes can target original instruction interiors (observed
# audio PC 0x10490 within the function at 0x10480). Register every reviewed
# aligned instruction as compiled code rather than use the microstep fallback.
entry_policy = os.environ.get('MACROSS_SPU_STATIC_ENTRIES', '').lower()
if entry_policy not in ('', 'audio', 'all'):
    raise SystemExit('MACROSS_SPU_STATIC_ENTRIES must be empty, audio, or all')
# Audio mixer (Nu-Sound 2 bnusCore): the ten DSP effect entries and its SPU
# malloc are reached only through function pointers in data tables, so the
# function finder misses them and every call fell to the microstep
# interpreter (0x10490 = malloc prologue + 0xC). A scan of the whole data
# segment found no other pointer targets (native/analysis/audio-spu-spec.md).
# Registering them is enough; the all-instructions policy is no longer needed.
AUDIO_TABLE_ENTRIES = (0x6320, 0x6D68, 0x7298, 0x7498, 0x7598, 0x75F8, 0x7678, 0x76F0,
                       0x78F8, 0x7AC8, 0x10480)
if digest.startswith('c068f107') and entry_policy == '':
    have = set()
    if '--extra-funcs' in args:
        i = args.index('--extra-funcs') + 1
        have = {int(a, 0) for a in args[i].split(',') if a.strip()}
        args[i] = ','.join(hex(a) for a in sorted(have | set(AUDIO_TABLE_ENTRIES)))
    else:
        args.extend(['--extra-funcs', ','.join(hex(a) for a in AUDIO_TABLE_ENTRIES)])
    print(f'[macross-spu] {digest[:12]} table-only entries registered: {len(AUDIO_TABLE_ENTRIES)}', flush=True)
if entry_policy == 'all' or (entry_policy == 'audio' and digest.startswith('c068f107')):
    root = Path(__file__).resolve().parents[2]
    sys.path.insert(0, str(root / 'tools'))
    sys.path.insert(0, str(root / 'tools' / 'analysis'))   # audit_spu_static_entries since the tools/ reorganisation
    from audit_spu_static_entries import prepare_entries
    if '--auto-functions' in args or '--functions' not in args:
        raise SystemExit('Static entry policy requires raw code and --functions')
    from find_spu_functions import parse_elf, text_image
    from spu_disasm import disassemble_spu
    from spu_lifter import compute_bi_r0_jumps
    expected_code, expected_base = text_image(image.read_bytes(), parse_elf(image.read_bytes())['phs'])
    if base != expected_base or code != expected_code:
        raise SystemExit('Raw SPU code/base does not match the validated ELF')
    # The SDK's r0 computed-jump classifier scans within function bounds.
    # One-instruction entries must not erase the original producer context:
    # preserve those classifications explicitly before replacing the bounds.
    prior = json.loads(Path(args[args.index('--functions') + 1]).read_text())
    prior_bounds = [(int(str(f['start']), 0), min(int(str(f['end']), 0), end))
                    for f in prior if int(str(f['start']), 0) < end]
    jumps = compute_bi_r0_jumps(disassemble_spu(code, base), prior_bounds)
    if '--force-indirect' in args:
        jumps.update(int(a, 0) for a in args[args.index('--force-indirect') + 1].split(',') if a)
        args[args.index('--force-indirect') + 1] = ','.join(hex(a) for a in sorted(jumps))
    elif jumps:
        args.extend(['--force-indirect', ','.join(hex(a) for a in sorted(jumps))])
    output = Path(args[args.index('--output') + 1])
    functions, audit = prepare_entries(image, output)
    audit['preserved_computed_r0_jump_sites'] = [hex(a) for a in sorted(jumps)]
    (output / 'static_entries.audit.json').write_text(json.dumps(audit, indent=2), encoding='utf-8')
    args[args.index('--functions') + 1] = str(functions)
    print(f'[macross-spu] static instruction entries: {audit["entry_count"]}', flush=True)

tool = Path(__file__).resolve().parents[2] / 'vendor/ps3recomp-master/tools/spu_lifter.py'
result = subprocess.run([sys.executable, '-X', 'utf8', str(tool), *args, '--code-end', hex(end)])
if result.returncode == 0:
    output = Path(args[args.index('--output') + 1])
    filename = args[args.index('--source-name') + 1] if '--source-name' in args else 'spu_recomp.c'
    generated = output / filename
    source, inserted = insert_queue_backoff(
        generated.read_text(encoding='utf-8'), prefix, code, base, digest)
    if inserted:
        print('[macross-spu] queue retry scheduling hook A134 + reset A140 guarded', flush=True)
    source, profiled = insert_function_perf(source, prefix, digest)
    if profiled:
        print(f'[macross-spu] optional sampled function scopes: {profiled}', flush=True)
    if inserted or profiled:
        generated.write_text(source, encoding='utf-8', newline='\n')
raise SystemExit(result.returncode)
