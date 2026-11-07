"""Guarded reproducible hooks for native battle-voice subtitles.

Default: inspect/plan only. --apply modifies four guarded generated PPU files.
Does not build, launch, edit game assets, install packs, or enable the bridge.
Bridge callbacks must preserve the complete guest context and trampoline state.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import uuid

ROOT = Path(__file__).resolve().parents[2]
INCLUDE = '#include "../battle_voice_subtitles.h"\n'
FILE_INCLUDE = '#include "../battle_voice_file_subtitles.h"\n'
FILE_SOURCE = 'ppu_recomp_004.cpp'
FUNCTIONS = {
    '000707B8': ('ppu_recomp_000.cpp', 'abd338ef57590b9638c83eb2056cbe22e9a567162f48d92adc5722263aa4ac2e'),
    '00070C7C': ('ppu_recomp_000.cpp', 'dec7c51f63a759c488f06f9a16b85223569da1e059acdaa09216b14c7acbd4b1'),
    '001CAF7C': (FILE_SOURCE, '37fa880d99ee5b515a2da5cdc626c464ffcb7e56865f1da4bfa7991cde215a65'),
    '00308E2C': ('ppu_recomp_007.cpp', '24cb8b7994a548ba3f3af825ac394e0475ed3b04848b38668e69c3c35a674214'),
    '00397670': ('ppu_recomp_008.cpp', 'cd5cf244d68ce317e358d22e168d58ecc1fdc44f7d334d56645957ad40da80d6'),
}
HOOKS = {
    '000707B8': [
        ('        ctx->lr = 0x00070834; func_0006FB68(ctx); DRAIN_TRAMPOLINE(ctx);\n',
         '        macross_battle_subtitles_selected(ctx, (uint32_t)(ctx->gpr[1] + 0x70)); // battle-voice: selected\n'),
        ('loc_000708A4:\n',
         '        macross_battle_subtitles_commit(ctx); // battle-voice: commit\n'),
    ],
    '00070C7C': [('void func_00070C7C(ppu_context* ctx) {\n',
                  '        macross_battle_subtitles_reset(ctx); // battle-voice: reset\n')],
    '001CAF7C': [
        ('void func_001CAF7C(ppu_context* ctx) {\n',
         '        macross_battle_subtitles_file_selected(ctx, (uint32_t)ctx->gpr[3]); // battle-voice: file-selected\n'),
        ('loc_001CAFF0:\n',
         '        macross_battle_subtitles_commit(ctx); // battle-voice: file-commit\n'),
    ],
    '00308E2C': [('void func_00308E2C(ppu_context* ctx) {\n',
                  '        macross_battle_subtitles_script_caption(ctx); // battle-voice: script-caption\n')],
    '00397670': [('void func_00397670(ppu_context* ctx) {\n',
                  '        macross_battle_subtitles_tick(ctx); // battle-voice: tick\n')],
}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def function_span(text, address):
    signature = 'void func_' + address + '(ppu_context* ctx) {'
    if text.count(signature) != 1:
        raise ValueError('Missing/ambiguous target function ' + address)
    start = text.index(signature)
    end = text.find('\n}\n', start)
    if end < 0:
        raise ValueError('Unbounded generated function ' + address)
    end += 2
    body = text[start:end]
    if re.search(r'^void func_', body[len(signature):], re.M):
        raise ValueError('Target function boundary drifted ' + address)
    return start, end, body


def transform(raw, filename):
    """Pure byte transform, accepting exact baseline or exact idempotent hooks."""
    text = raw.decode('utf8')
    newline = '\r\n' if '\r\n' in text else '\n'
    if newline == '\r\n' and '\n' in text.replace('\r\n', ''):
        raise ValueError('Mixed newline style in ' + filename)
    text = text.replace('\r\n', '\n')
    # Compose only with the two separately reviewed diagnostic observer lines.
    probe_include = '#include "../battle_voice_acceptance_probe.h"\n'
    probe_request = '        macross_battle_voice_probe_request(ctx, (uint32_t)ctx->gpr[3]); // acceptance-probe: request\n'
    probe_open = '        macross_battle_voice_probe_open(ctx, (uint32_t)ctx->gpr[29], (int32_t)ctx->gpr[3]); // acceptance-probe: open-result\n'
    if probe_include in text or 'acceptance-probe:' in text:
        if filename != FILE_SOURCE or any(text.count(x) != 1 for x in (probe_include, probe_request, probe_open)):
            raise ValueError('Partial/foreign acceptance probe additions')
        request_anchor = 'void func_001CAF7C(ppu_context* ctx) {\n'
        open_anchor = '        ctx->lr = 0x001C4C18; func_00704444(ctx); DRAIN_TRAMPOLINE(ctx);\n        ctx->gpr[2] = vm_read64(ctx->gpr[1] + 0x28);\n'
        if request_anchor+probe_request not in text or open_anchor+probe_open not in text:
            raise ValueError('Acceptance probe position drift')
        if text.split('\n', 2)[1]+'\n' != probe_include:
            raise ValueError('Acceptance probe include position drift')
        stripped = text.replace(probe_include, '', 1).replace(probe_request, '', 1).replace(probe_open, '', 1)
        if digest(function_span(stripped, '001C4BB0')[2].encode()) != '3b721c47124bccece3961d28c7502e51f58ebb7c8a9495ffeb13a0a621054665':
            raise ValueError('Acceptance probe original open function drift')
        result, proof = transform(stripped.replace('\n', newline).encode(), filename)
        restored = result.decode().replace('\r\n', '\n')
        restored = restored.replace(request_anchor, request_anchor+probe_request, 1).replace(open_anchor, open_anchor+probe_open, 1)
        first = restored.find('\n')+1
        restored = restored[:first]+probe_include+restored[first:]
        output = restored.replace('\n', newline).encode()
        proof.update(source_sha256=digest(raw), result_sha256=digest(output), acceptance_probe_preserved=True)
        return output, proof
    targets = [a for a, (f, _) in FUNCTIONS.items() if f == filename]
    if not targets:
        raise ValueError('Unreviewed generated file')
    include = FILE_INCLUDE if filename == FILE_SOURCE else INCLUDE
    other_include = INCLUDE if filename == FILE_SOURCE else FILE_INCLUDE
    if other_include in text:
        raise ValueError('Wrong battle-voice header for ' + filename)
    expected_hook_lines = [line for a in targets for _, line in HOOKS[a]]
    present = [text.count(line) for line in expected_hook_lines]
    if any(n not in (0, 1) for n in present):
        raise ValueError('Duplicated battle-voice hook')
    installed = all(present)
    if any(present) and not installed:
        raise ValueError('Partial battle-voice hook state')
    if text.count(include) != int(installed):
        raise ValueError('Include/hook state mismatch')
    # Strip precisely our additions; verify each complete original function SHA.
    original = text
    if installed:
        original = original.replace(include, '', 1)
        for line in expected_hook_lines:
            original = original.replace(line, '', 1)
    changes = []
    modified = original
    for address in targets:
        start, end, body = function_span(modified, address)
        if digest(body.encode('utf8')) != FUNCTIONS[address][1]:
            raise ValueError('Original function changed; review before patching ' + address)
        if address == '000707B8':
            # This exact baseline has one normal return, in this common epilogue.
            # Both normal branches converge after the filename has been selected.
            if body.count('return;') != 1 or body.count('loc_000708A4:') != 1:
                raise ValueError('Voice return coverage changed')
            capture_anchor = HOOKS[address][0][0]
            if not body.index(capture_anchor) < body.index('loc_000708A4:') < body.index('return;'):
                raise ValueError('Capture/commit/return order changed')
        if address == '001CAF7C':
            # Both SSO branches and the optional string destructor converge on
            # this epilogue after the actual native voice-channel start request.
            start_anchor = 'ctx->lr = 0x001CAFD4; func_001C4BB0(ctx); DRAIN_TRAMPOLINE(ctx);'
            if (body.count('return;') != 1 or body.count('loc_001CAFF0:') != 1 or
                body.count(start_anchor) != 1):
                raise ValueError('Filename voice return coverage changed')
            if not body.index(start_anchor) < body.index('loc_001CAFF0:') < body.index('return;'):
                raise ValueError('File start/commit/return order changed')
        new_body = body
        for anchor, line in HOOKS[address]:
            if new_body.count(anchor) != 1:
                raise ValueError('Missing/ambiguous exact hook anchor ' + address)
            new_body = new_body.replace(anchor, anchor+line, 1)
        modified = modified[:start] + new_body + modified[end:]
        changes.append(dict(address='0x'+address, original_function_sha256=FUNCTIONS[address][1],
                            hooks=[line.strip() for _, line in HOOKS[address]]))
    anchor = '#include "ppu_recomp.h"\n'
    if modified.count(anchor) != 1:
        raise ValueError('Missing/ambiguous generated header')
    modified = modified.replace(anchor, anchor+include, 1)
    # No unrelated text modification is possible: reconstruction minus additions
    # must exactly equal the normalized input with those same additions removed.
    restored = modified.replace(include, '', 1)
    for line in expected_hook_lines:
        if modified.count(line) != 1:
            raise ValueError('Final hook count mismatch')
        restored = restored.replace(line, '', 1)
    if restored != original or (installed and modified != text):
        raise ValueError('Unexpected source change outside exact additions')
    output = modified.replace('\n', newline).encode('utf8')
    return output, dict(file=filename, already_patched=installed, source_sha256=digest(raw),
                        result_sha256=digest(output), functions=changes,
                        unrelated_source_changed=False)


def plan(directory):
    directory = Path(directory).resolve()
    changes = []
    report = []
    for filename in sorted({f for f, _ in FUNCTIONS.values()}):
        path = directory/filename
        raw = path.read_bytes()
        output, proof = transform(raw, filename)
        changes.append((path, raw, output))
        report.append(proof)
    old_states = {r['already_patched'] for r in report if r['file'] != FILE_SOURCE}
    file_state = next(r['already_patched'] for r in report if r['file'] == FILE_SOURCE)
    # Exact old three-file installation plus the newly reviewed file adapter is
    # the permitted migration. Partial states within the old installation remain
    # rejected, as does a filename-only partial installation.
    if len(old_states) != 1 or (file_state and not next(iter(old_states))):
        raise ValueError('Unreviewed mixed patched/unpatched generated files')
    migration = next(iter(old_states)) and not file_state
    return changes, dict(status='GUARDED_PLAN', files=report,
                        hooks=7, target_functions=5, normal_voice_returns=1,
                        normal_file_voice_returns=1,
                        extending_existing_stem_hooks=migration,
                        capture_before_commit=True, all_normal_voice_returns_commit=True,
                        changes='One matching include per generated file and seven callback lines only',
                        queue_rng_guest_instructions_changed=False,
                        callbacks_require_complete_context_preservation=True,
                        bridge_enabled_by_patcher=False, game_launched=False, packs_modified=False)


def self_test(directory):
    """Pure transformations only; no generated files or game data are written."""
    cases = 0
    for filename in sorted({f for f, _ in FUNCTIONS.values()}):
        original_raw = (Path(directory)/filename).read_bytes()
        normalized = original_raw.decode('utf8').replace('\r\n', '\n')
        include = FILE_INCLUDE if filename == FILE_SOURCE else INCLUDE
        # Recover only our exact existing additions, then independently exercise
        # the first installation and the idempotent second installation.
        baseline = normalized.replace(include, '', 1)
        targets = [a for a, (f, _) in FUNCTIONS.items() if f == filename]
        for address in targets:
            for _, line in HOOKS[address]:
                baseline = baseline.replace(line, '', 1)
        first, proof = transform(baseline.encode('utf8'), filename)
        second, repeated = transform(first, filename)
        if proof['already_patched'] or not repeated['already_patched'] or first != second:
            raise AssertionError('Installation/idempotence failed: ' + filename)
        cases += 2
        # A partial hook must never be silently repaired into a plausible state.
        line = HOOKS[targets[0]][0][1]
        bad = first.decode('utf8').replace(line, '', 1)
        try:
            transform(bad.encode('utf8'), filename)
        except ValueError:
            cases += 1
        else:
            raise AssertionError('Accepted partial hooks: ' + filename)
        # Whole-function SHA, not an isolated substring, guards the original code.
        start, end, body = function_span(baseline, targets[0])
        marker = 'ctx->gpr[0] = ctx->lr;'
        if marker not in body:
            raise AssertionError('Fixture mutation anchor missing')
        bad_body = body.replace(marker, 'ctx->gpr[0] = ctx->lr ^ 1;', 1)
        try:
            transform((baseline[:start]+bad_body+baseline[end:]).encode('utf8'), filename)
        except ValueError:
            cases += 1
        else:
            raise AssertionError('Accepted changed guest instruction: ' + filename)
    # Planning the live source tree is read-only and exercises the permitted
    # old-hooks-present/new-file-hooks-absent migration as it currently exists.
    _, report = plan(directory)
    if report['hooks'] != 7 or report['target_functions'] != 5 or report['normal_file_voice_returns'] != 1:
        raise AssertionError('Incomplete filename hook plan')
    return dict(status='PASS', cases=cases+1, files=4,
                migration=report['extending_existing_stem_hooks'],
                generated_files_written=False, game_started=False)


def apply(changes):
    """Recheck originals, write independent temps, replace; rollback on failure."""
    temps = []
    replaced = []
    try:
        for path, raw, output in changes:
            if path.read_bytes() != raw:
                raise ValueError('Concurrent source modification: '+path.name)
            if raw == output:
                continue
            tmp=path.with_name(path.name+'.battle-voice-'+uuid.uuid4().hex+'.tmp')
            tmp.write_bytes(output)
            temps.append((tmp,path,raw))
        # Recheck all inputs before replacing any output.
        if any(path.read_bytes()!=raw for path,raw,_ in changes):
            raise ValueError('Source changed before publication')
        for tmp,path,raw in temps:
            os.replace(tmp,path)
            replaced.append((path,raw))
    except BaseException:
        for path,raw in reversed(replaced):
            path.write_bytes(raw)
        raise
    finally:
        for tmp,_,_ in temps:
            if tmp.exists():
                tmp.unlink()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--recompiled-dir',type=Path,default=ROOT/'native/macross30/recompiled')
    parser.add_argument('--report',type=Path)
    parser.add_argument('--apply',action='store_true')
    parser.add_argument('--self-test',action='store_true')
    args=parser.parse_args()
    directory=args.recompiled_dir.resolve()
    if ROOT not in directory.parents:
        raise ValueError('Generated source directory must be inside workspace')
    if args.self_test:
        if args.apply or args.report:
            raise ValueError('--self-test does not apply or publish a report')
        print(json.dumps(self_test(directory), indent=2))
        return
    changes,report=plan(directory)
    output=None
    if args.report:
        output=args.report.resolve()
        if ROOT not in output.parents or output in {p for p,_,_ in changes}:
            raise ValueError('Report must be a separate workspace artifact')
    if args.apply:
        apply(changes)
    report['applied']=args.apply
    if output is not None:
        output.parent.mkdir(parents=True,exist_ok=True)
        output.write_bytes((json.dumps(report,indent=2)+'\n').encode())
    print(json.dumps(report,indent=2))


if __name__=='__main__':
    main()
