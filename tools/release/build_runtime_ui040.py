"""Relink accepted m29 with updated native UI and without its F8 measurement unit.

All other accepted link inputs are hash checked and reused read-only. The
accepted EXE and other agents' worktrees are never modified. Optional verified
integration sources add the reviewed mouse controls alongside m29 rumble.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
ACCEPTED = 'f2776236ec0606e457495445e5fa1c2f245ea087e1c39565ea7a956354545332'


def sha(path):
    with Path(path).open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def main(parent, output, refresh_ui=False, integration_source=None):
    parent, output = Path(parent).resolve(), Path(output).resolve()
    if not output.is_relative_to(ROOT / 'build/releases/0.4.0'):
        raise ValueError('Require new independent 0.4.0 output directory')
    if output.exists():
        if not refresh_ui or not (output / 'BUILD-RESULT.json').is_file():
            raise ValueError('Existing output requires a verified prior result and --refresh-ui')
        previous = json.loads((output / 'BUILD-RESULT.json').read_text('utf8'))
        if previous.get('base_exe_sha256') != ACCEPTED or not previous.get('passed'):
            raise ValueError('Existing output is not this task\'s accepted-parent candidate')
    if sha(parent / 'Macross30Recompiled.exe') != ACCEPTED:
        raise ValueError('Not the accepted m29 executable')
    exact = json.loads((parent / 'exact-inputs.json').read_text('utf8'))
    result = json.loads((parent / 'build-result.json').read_text('utf8'))
    hashes = dict(exact['same_objects'])
    hashes.update({k: v['m29'] for k, v in exact['changed_objects'].items()})
    for name, expected in hashes.items():
        if sha(parent / 'objects' / name) != expected:
            raise ValueError('Accepted object changed: ' + name)
    for name, expected in exact['shared_absolute_link_inputs'].items():
        if sha(Path(name)) != expected:
            raise ValueError('Accepted shared link input changed: ' + Path(name).name)
    output.mkdir(parents=True, exist_ok=refresh_ui)
    src = output / 'src'
    shutil.copytree(parent / 'src/rumble', src, dirs_exist_ok=refresh_ui)
    video = ROOT / 'vendor/ps3recomp-master/libs/video'
    for name in ['macross_ui_language.h', 'macross_ui_external.h', 'macross_ui_strings.def',
                 'macross_ui_ru.def', 'macross_ui_es.def']:
        shutil.copyfile(video / name, src / name)
    header = src / 'macross_ui_language.h'
    header.write_text(header.read_text('utf8').replace('"../system/savedlg_strings.def"', '"savedlg_strings.def"'), encoding='utf8')
    shutil.copyfile(ROOT / 'vendor/ps3recomp-master/libs/system/savedlg_strings.def', src / 'savedlg_strings.def')
    if sha(src / 'rsx_null_backend.c') != result['frame_option']['source_sha256']:
        raise ValueError('Accepted options implementation changed unexpectedly')
    merged_inputs = {}
    if integration_source:
        integration_source = Path(integration_source).resolve()
        merge = json.loads((integration_source / 'MERGE-RESULT.json').read_text('utf8'))
        if not merge['passed']:
            raise ValueError('Mouse merge source not verified')
        for name, row in merge['files'].items():
            if Path(name).name != name or sha(integration_source/name) != row['merged_sha256']:
                raise ValueError('Merged source identity changed: '+name)
            shutil.copyfile(integration_source/name, src/name)
            merged_inputs[name] = row['merged_sha256']
    objects = output / 'objects'
    objects.mkdir(exist_ok=refresh_ui)
    cmd = result['frame_option']['command'][:]
    cmd.insert(1, '/I' + str(src))
    for i, arg in enumerate(cmd):
        if arg.startswith('/Fo'):
            cmd[i] = '/Fo' + str(objects / 'rsx_null_backend.c.obj')
        elif arg.startswith('/Fd'):
            cmd[i] = '/Fd' + str(objects / 'compile.pdb')
        elif arg == str(parent / 'src/rumble/rsx_null_backend.c'):
            cmd[i] = str(src / 'rsx_null_backend.c')
    with (output / 'compile.log').open('w', encoding='utf8') as log:
        subprocess.run(cmd, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
    if integration_source:
        cmd = result['gameplay_rumble']['pad_command'][:]
        cmd.insert(1, '/I'+str(src))
        for i, arg in enumerate(cmd):
            if arg.startswith('/Fo'):
                cmd[i] = '/Fo'+str(objects/'cellPad.c.obj')
            elif arg == str(parent/'src/rumble/cellPad.c'):
                cmd[i] = str(src/'cellPad.c')
        with (output/'compile-pad.log').open('w', encoding='utf8') as log:
            subprocess.run(cmd, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
        if 'frame_pacing.cpp' in merged_inputs:
            cmd = result['frame_pacing']['command'][:]
            cmd.insert(1, '/I'+str(src))
            for i,arg in enumerate(cmd):
                if arg.startswith('/Fo'):
                    cmd[i] = '/Fo'+str(objects/'frame_pacing.cpp.obj')
                elif arg == str(parent/'src/frame_pacing.cpp'):
                    cmd[i] = str(src/'frame_pacing.cpp')
            with (output/'compile-pacing.log').open('w',encoding='utf8') as log:
                subprocess.run(cmd,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True)
    # New translated placement CSVs also exercise the title's same-encoding
    # converter sentinel. Freeze and compile the corrected bridge independently
    # of the accepted parent's read-only shared object.
    for name in ['l10n_bridge.cpp', 'l10n_bridge.h']:
        shutil.copyfile(ROOT/'native/macross30'/name, src/name)
        merged_inputs[name] = sha(src/name)
    cmd = [result['frame_pacing']['command'][0], '/nologo', '/std:c++20',
           '/O1', '-MD', '/DNDEBUG', '/EHs', '/EHc-',
           '/I'+str(ROOT/'native/macross30'),
           '/Fo'+str(objects/'l10n_bridge.cpp.obj'), '-c', str(src/'l10n_bridge.cpp')]
    with (output/'compile-l10n.log').open('w',encoding='utf8') as log:
        subprocess.run(cmd,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True)
    rsp = (parent / 'link.rsp').read_text('utf8').splitlines()
    excluded = [line for line in rsp if Path(line.strip('"')).name.lower() == 'measure_window.cpp.obj']
    if len(excluded) != 1:
        raise ValueError('Expected exactly one F8 measurement link unit')
    rsp = [line for line in rsp if line not in excluded]
    old_l10n = ROOT/'native/build/CMakeFiles/Macross30Recompiled.dir/l10n_bridge.cpp.obj'
    if sum(Path(line.strip('"')) == old_l10n for line in rsp) != 1:
        raise ValueError('Expected one accepted character conversion bridge')
    rsp = [str(objects/'l10n_bridge.cpp.obj') if Path(line.strip('"')) == old_l10n else line for line in rsp]
    old = str(parent / 'objects/rsx_null_backend.c.obj')
    if sum(line.strip('"') == old for line in rsp) != 1:
        raise ValueError('Expected one accepted options object')
    rsp = [str(objects / 'rsx_null_backend.c.obj') if line.strip('"') == old else line for line in rsp]
    if integration_source:
        old_pad = str(parent/'objects/cellPad.c.obj')
        if sum(line.strip('"') == old_pad for line in rsp) != 1:
            raise ValueError('Expected one accepted pad object')
        rsp = [str(objects/'cellPad.c.obj') if line.strip('"') == old_pad else line for line in rsp]
        if 'frame_pacing.cpp' in merged_inputs:
            old_pacing = str(parent/'objects/frame_pacing.cpp.obj')
            if sum(line.strip('"') == old_pacing for line in rsp) != 1:
                raise ValueError('Expected one accepted frame pacing object')
            rsp = [str(objects/'frame_pacing.cpp.obj') if line.strip('"') == old_pacing else line for line in rsp]
    if any('measure_window' in line.lower() for line in rsp):
        raise ValueError('Measurement module still in release response')
    (output / 'link.rsp').write_text('\n'.join(rsp) + '\n', encoding='utf8')
    original = result['link_command']
    split = original.index('--')
    link = original[split + 1:]
    for i, arg in enumerate(link):
        if arg.startswith('@'):
            link[i] = '@' + str(output / 'link.rsp')
        elif arg.lower().startswith(('/out:', '/implib:', '/pdb:', '/map:')):
            prefix, old_path = arg.split(':', 1)
            link[i] = prefix + ':' + str(output / Path(old_path).name)
    link += ['/PDBALTPATH:%_PDB%', '/lldltocache:' + str(output / 'lto-cache')]
    print('Accepted m29 link inputs verified; relinking without F8 measurement module.', flush=True)
    with (output / 'link.log').open('w', encoding='utf8') as log:
        subprocess.run(link, cwd=output, stdout=log, stderr=subprocess.STDOUT, check=True)
    map_path = output / 'Macross30Recompiled.map'
    map_text = map_path.read_text('utf8', errors='replace')
    if 'macross_measure_trigger' in map_text or 'measure_window.cpp.obj' in map_text:
        raise ValueError('F8 measurement symbols remain in linked map')
    exe = output / 'Macross30Recompiled.exe'
    data = exe.read_bytes()
    if any(marker in data for marker in [b'[measure] ready', b'[measure] done window=', b'measure.go']):
        raise ValueError('Measurement activation markers remain in executable')
    for name, expected in hashes.items():
        if sha(parent / 'objects' / name) != expected:
            raise ValueError('Accepted parent object modified during relink')
    report = dict(passed=True, base_exe_sha256=ACCEPTED, exe_sha256=sha(exe),
                  changed_objects=['rsx_null_backend.c.obj','l10n_bridge.cpp.obj']+(['cellPad.c.obj'] if integration_source else [])+(['frame_pacing.cpp.obj'] if 'frame_pacing.cpp' in merged_inputs else []), removed_objects=['measure_window.cpp.obj'],
                  accepted_objects_verified=len(hashes), shared_inputs_verified=len(exact['shared_absolute_link_inputs']),
                  measurement_symbols_absent=True, measurement_activation_markers_absent=True,
                  f8_key_watch_removed=True, measure_go_activation_removed=True,
                  accepted_parent_unmodified=True, mouse_merge_complete=bool(integration_source),
                  merged_inputs=merged_inputs,
                  native_ui_inputs={n:sha(src/n) for n in ['macross_ui_language.h','macross_ui_external.h',
                                    'macross_ui_strings.def','macross_ui_ru.def','macross_ui_es.def']},
                  game_started=False, visual_check='pending', deployment_ready=False)
    (output / 'BUILD-RESULT.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf8')
    print(json.dumps(report, ensure_ascii=False), flush=True)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--parent', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--refresh-ui', action='store_true')
    p.add_argument('--integration-source', type=Path)
    a = p.parse_args()
    main(a.parent, a.output, a.refresh_ui, a.integration_source)
