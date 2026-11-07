"""Content-based generation cache. Never treats a failed task as complete."""
import hashlib
import json
import inspect
import os
from pathlib import Path
import subprocess
import sys


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_changed(path, data):
    path = Path(path)
    if path.exists() and path.read_bytes() == data:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + '.pending')
    temporary.write_bytes(data)
    os.replace(temporary, path)
    return True


class Tasks:
    def __init__(self, root):
        self.root = Path(root)
        self.path = self.root / 'native/analysis/generation-cache.json'
        self.state = json.loads(self.path.read_text()) if self.path.exists() else {'schema': 1, 'tasks': {}}
        self.legacy = {}
        legacy = self.root / 'legacy-source-manifest.json'
        marker = self.root / 'local-generation.ready'
        if legacy.exists() and marker.exists() and marker.read_text().strip() == '44d7112cbb5527314eb6192e59d861dacfcd4099f301a6e82ffc1681b0782b5b':
            info = json.loads(legacy.read_text(encoding='utf8'))
            if info.get('verified_origin') == '5759290d3b26ab508760a72d32cbe5de044c0d7c5e5b0310d0709f792c319a7c':
                self.legacy = {row['path']: row['sha256'] for row in info['files']}

    def legacy_matches(self, path, sha):
        if self.legacy.get(path) == sha:
            return True
        if path == 'tools/local_build/apply_hooks.py':
            # This release only adds a directory selector to the old exact hook
            # engine. Prove that removing that selector yields the original
            # manifest bytes; any other engine change requires regeneration.
            text = (self.root / path).read_text(encoding='utf8')
            text = text.replace('def apply(root=ROOT, directories=None):', 'def apply(root=ROOT):')
            text = text.replace("        if directories is not None and row.get('directory', 'recompiled') not in directories:\n            continue\n", '')
            return self.legacy.get(path) in {hashlib.sha256(text.encode()).hexdigest(),
                                           hashlib.sha256(text.replace('\n', '\r\n').encode()).hexdigest()}
        return False

    def run(self, name, dependencies, parameters, outputs, action, adopt=False):
        dependencies = {str(Path(p).relative_to(self.root)).replace('\\', '/'): digest(p) for p in dependencies}
        signature = hashlib.sha256(json.dumps([1, dependencies, parameters], sort_keys=True, default=lambda value: {'bytes': value.hex()} if isinstance(value, bytes) else str(value)).encode()).hexdigest()
        old = self.state['tasks'].get(name)
        if old and old['signature'] == signature and old['outputs'] and all(
                (self.root / p).is_file() and digest(self.root / p) == h for p, h in old['outputs'].items()):
            print('LOCAL_CACHE_HIT: ' + name, flush=True)
            return False
        existing = list(outputs())
        imported = (not old and adopt and existing and all(p.is_file() for p in existing) and self.legacy and
                    all(self.legacy_matches(p, h) for p, h in dependencies.items() if not p.startswith('native/macross30/raw_jobs/')))
        before = {p: (digest(p), p.stat().st_atime_ns, p.stat().st_mtime_ns) for p in existing if p.is_file()}
        if imported and name == 'geom2':
            imported = digest(self.root / 'native/macross30/spu_gameplay/geom2/spu_recomp.c') == '819b73fbce196edf00f500dd5fb89f015019a7b55536d4fdabdaf932e2cf1178'
        if not imported:
            print('LOCAL_GENERATE: ' + name, flush=True)
            action()
        files = list(outputs())
        if not files or not all(p.is_file() for p in files):
            raise ValueError('Incomplete generation task: ' + name)
        for p, (sha, access, modified) in before.items():
            if p.exists() and digest(p) == sha:
                os.utime(p, ns=(access, modified))
        self.state['tasks'][name] = dict(signature=signature, outputs={p.relative_to(self.root).as_posix(): digest(p) for p in files})
        write_changed(self.path, (json.dumps(self.state, indent=2) + '\n').encode())
        print(('LOCAL_CACHE_IMPORTED: ' if imported else 'LOCAL_GENERATED: ') + name, flush=True)
        return not imported


def generate(root):
    root = Path(root).resolve()
    sys.path.insert(0, str(root / 'tools/build'))
    import importlib.util
    def module(name, path):
        spec = importlib.util.spec_from_file_location(name, root / path)
        value = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(value)
        value.ROOT = root
        return value
    gameplay = module('incremental_gameplay', 'tools/build/lift_gameplay_jobs.py')
    hooks = module('incremental_hooks', 'tools/local_build/apply_hooks.py')
    recipe = json.loads((root / 'tools/local_build/ppu_hooks.json').read_text(encoding='utf8'))
    tasks = Tasks(root)
    if hashlib.sha256(inspect.getsource(generate).encode()).hexdigest() != LEGACY_PLAN_SHA256:
        tasks.legacy = {}
    sdk = list((root / 'vendor/ps3recomp-master/tools').glob('spu_*.py'))
    sdk += [root / 'vendor/ps3recomp-master/tools/find_spu_functions.py']
    original = root / 'analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf'
    def call(*args):
        subprocess.run([sys.executable, '-X', 'utf8', *args], cwd=root, check=True)
    def rowset(directory):
        return [row for row in recipe['files'] if row.get('directory', 'recompiled') == directory]
    def validate_hooks(directory):
        return all((root / 'native/macross30' / directory / row['file']).is_file() and
                   hooks.digest((root / 'native/macross30' / directory / row['file']).read_text(encoding='utf8')) == row['output_sha256']
                   for row in rowset(directory))
    def ppu():
        call('vendor/ps3recomp-master/tools/ppu_loader.py', str(original), '-o', 'native/analysis')
        call('vendor/ps3recomp-master/tools/ppu_lifter.py', str(original), '--functions', 'native/analysis/EBOOT.functions.json',
             '--hle-stubs', 'native/analysis/EBOOT.imports.json', '--code-end', '0x8ddf4c', '--chunks', '96', '--jobs', '4', '--output', 'native/macross30/recompiled')
        hooks.apply(root, {'recompiled'})
    ppu_deps = list((root / 'vendor/ps3recomp-master/tools').glob('ppu_*.py'))
    ppu_deps += [root / 'tools/local_build/apply_hooks.py']
    tasks.run('PPU', ppu_deps, [digest(original), rowset('recompiled'), 96, '0x8ddf4c', inspect.getsource(ppu)],
              lambda: [root / 'native/macross30/recompiled' / name for name in ['ppu_recomp_table.cpp', 'ppu_recomp.h', *('ppu_recomp_%03d.cpp' % i for i in range(96))]], ppu,
              adopt=validate_hooks('recompiled'))
    for name, script, raw in [('notify', 'lift_native_job.py', '4c091e2c0bd5c570'), ('graphics', 'lift_graphics_job.py', '0ae6346f73bc04a7')]:
        folder = root / ('native/macross30/spu_' + name)
        deps = [root / ('tools/build/' + script), root / ('native/macross30/raw_jobs/job_' + raw + '.bin'), *sdk]
        if name == 'notify':
            deps.append(root / 'tools/build/extract_native_job.py')
        tasks.run(name, deps, [], lambda folder=folder: [folder / 'spu_recomp.c', folder / 'spu_recomp.h', folder / 'report.json'],
                  lambda script=script: call('tools/build/' + script), adopt=True)
    for job in gameplay.JOBS:
        name = job['label']
        directory = 'spu_gameplay/' + name
        folder = root / 'native/macross30' / directory
        deps = [root / 'tools/build/lift_gameplay_jobs.py', root / ('native/macross30/raw_jobs/job_' + job['fingerprint'] + '.bin'), *sdk]
        extra = rowset(directory)
        if digest(root / ('native/macross30/raw_jobs/job_' + job['fingerprint'] + '.bin')) != job['sha256']:
            raise ValueError('Original job identity mismatch: ' + name)
        if name == 'geom1':
            deps.append(root / 'tools/local_build/apply_hooks.py')
        if name == 'geom2':
            deps.append(root / 'tools/build/patch_geom2_local_dispatch.py')
        def action(job=job, name=name, folder=folder, directory=directory):
            gameplay.lift(job, folder, merge_chunks=name not in ('jobcrt14', 'jobcrt14_b', 'jobcrt14_d'), max_group_insns=4000)
            if name == 'geom2':
                # The patch tool checks a frozen baseline; a new baseline replaces only its own saved candidate.
                backup = root / 'native/analysis/geom2-local-dispatch-candidate/baseline-spu_recomp.c'
                if backup.exists() and backup.read_bytes() != (folder / 'spu_recomp.c').read_bytes():
                    backup.unlink()
                call('tools/build/patch_geom2_local_dispatch.py', '--apply')
            if name == 'geom1':
                hooks.apply(root, {directory})
        tasks.run(name, deps, [job, extra, name not in ('jobcrt14', 'jobcrt14_b', 'jobcrt14_d'), 4000, inspect.getsource(action)],
                  lambda folder=folder: [folder / 'spu_recomp.c', folder / 'spu_recomp.h'], action,
                  adopt=validate_hooks(directory))
    def registration():
        lines = ['#include "spu_workload.h"', '#include "spu_context.h"', 'extern void spu_begin_image(int);']
        for job in gameplay.JOBS:
            prefix = 'macross_' + job['label'] + '_'
            lines.extend([f'extern void {prefix}spu_recomp_register(void);', f'extern void {prefix}spu_func_00000000(spu_context*);'])
        lines.extend(['void macross_gameplay_register(void)', '{'])
        for job in gameplay.JOBS:
            prefix = 'macross_' + job['label'] + '_'
            lines.extend([f'    spu_begin_image({job["image_id"]});', f'    {prefix}spu_recomp_register();',
                          f'    spu_workload_register_img(0x{job["fingerprint"].upper()}ULL,',
                          f'        {prefix}spu_func_00000000,{job["image_id"]},"Macross {job["source_component"]} captured original job");'])
        lines.extend(['    spu_begin_image(0);', '}', ''])
        folder = root / 'native/macross30/spu_gameplay'
        write_changed(folder / 'register.c', '\n'.join(lines).encode())
        write_changed(folder / 'register.h', b'#pragma once\n#ifdef __cplusplus\nextern "C" {\n#endif\nvoid macross_gameplay_register(void);\n#ifdef __cplusplus\n}\n#endif\n')
    tasks.run('SPU registration', [root / 'tools/build/lift_gameplay_jobs.py'], [gameplay.JOBS, inspect.getsource(registration)],
              lambda: [root / 'native/macross30/spu_gameplay/register.c', root / 'native/macross30/spu_gameplay/register.h'], registration, adopt=True)
    post_deps = [root / ('tools/local_build/' + name) for name in ('posttask.py', 'posttask.json', 'posttask_guard.txt', 'boundary_perf.h', 'code_signatures.py')]
    tasks.run('Posttask kernels', [*post_deps, *sdk], digest(original),
              lambda: [root / 'native/macross30/spu_posttask_native' / name for name in ['guard.c', 'boundary_perf.h', *(f'family{family}.{suffix}' for family in (0,1,2,4,6,8,9,10,11) for suffix in ('c','h'))]],
              lambda: call('tools/local_build/posttask.py'), adopt=True)

# Exact reviewed plan that is compatible with the published 0.2.0 generation.
LEGACY_PLAN_SHA256 = '7b102b7cb61209b8e68043594bc1a7ecb7d53495b1ab90ab143fcb835d9b1441'
