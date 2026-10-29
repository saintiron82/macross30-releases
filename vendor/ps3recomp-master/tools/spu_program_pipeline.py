"""Validated discovery and fresh generation for build_spu_workloads --input.

Uses the existing detector, lifter CLI and registry emitter. Owns its entire
output directory; manifests and logs remain available after failed generation.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

from spu_elf import fnv1a64, scan_spu_candidates
from spu_generator_dependencies import generator_dependencies


def sha256(buf):
    return hashlib.sha256(buf).hexdigest()


CACHE_SCHEMA = 2


def reparse_path(path):
    """Reject Windows junctions as well as ordinary symlinks before resolving."""
    for component in (path, *path.parents):
        try:
            info = component.lstat()
        except FileNotFoundError:
            continue
        if component.is_symlink() or getattr(info, 'st_file_attributes', 0) & 0x400:
            return True
    return False


def generation_key(args, inputs, destination, registry):
    """Content identities only; never trust source mtimes or wrapper-only hashes.

    Use the same supported generator closure as CMake, including CLI targets,
    conditional imports and the Macross audited entry helper. Unknown external
    wrappers remain uncacheable. Inputs/options/output validation is unchanged.
    """
    tools = Path(__file__).resolve().parent
    lifter = Path(args.lifter).resolve()
    dependencies = generator_dependencies(lifter)
    if dependencies is None:
        return None
    return dict(schema=CACHE_SCHEMA,
                inputs=[dict(path=p, size=Path(p).stat().st_size,
                             sha256=sha256(Path(p).read_bytes())) for p in inputs],
                tools={str(p.resolve()): sha256(p.read_bytes()) for p in sorted(dependencies)},
                python=dict(executable=str(Path(sys.executable).resolve()), version=sys.version),
                options={k: v for k, v in sorted(vars(args).items())
                         if k not in ('input', 'lifted', 'out')},
                destination=str(destination), registry=str(registry),
                environment={'MACROSS_SPU_STATIC_ENTRIES':
                             os.environ.get('MACROSS_SPU_STATIC_ENTRIES', '').lower()})


def valid_cached_generation(destination, key):
    """Fail closed on incomplete coverage, manifest drift, or any output change."""
    if key is None or reparse_path(destination):
        return False
    try:
        manifest = destination / 'inventory.json'
        encoded = manifest.read_bytes()
        report = json.loads(encoded)
        if (report.get('generator') != 'build_spu_workloads' or report.get('schema_version') != 1
                or report.get('complete') is not True or report.get('errors') != []
                or report.get('cache_key') != key or report.get('inputs') != key['inputs']):
            return False
        programs = report['programs']
        if not programs or any(p.get('status') != 'registered' for p in programs):
            return False
        expected = dict(report['outputs'])
        if 'sources.cmake' not in expected or Path(key['registry']).name not in expected:
            return False
        for program in programs:
            outputs = program['outputs']
            name = program['name']
            for leaf in ('image.elf', 'code.bin', 'functions.json', 'spu_recomp.c',
                         'spu_recomp.h', 'lift.json', 'lift.log'):
                if name + '/' + leaf not in outputs:
                    return False
            if outputs[name + '/image.elf'] != program['sha256']:
                return False
            if expected.keys() & outputs.keys():
                return False
            expected.update(outputs)
        actual = set()
        for path in destination.rglob('*'):
            if reparse_path(path):
                return False
            if path.is_file():
                actual.add(path.relative_to(destination).as_posix())
        if actual != set(expected) | {'inventory.json'}:
            return False
        for relative, digest in expected.items():
            safe = Path(relative)
            if safe.is_absolute() or '..' in safe.parts or '\\' in relative:
                return False
            if sha256((destination / safe).read_bytes()) != digest:
                return False
        # A concurrent publisher must not change the manifest during validation.
        return manifest.read_bytes() == encoded
    except (OSError, ValueError, KeyError, TypeError):
        return False


def code_ranges(info):
    # The loader applies every PT_LOAD in header order, including zero filling.
    # Lifting a segment in isolation is unsafe if another load can replace its
    # bytes. Treat any overlapping memory layout as unsupported until generation
    # explicitly models the final deployed local store. Adjacent/empty loads
    # remain valid.
    loads = sorted((p['addr'], p['addr'] + p['memsz']) for p in info['loads'] if p['memsz'])
    if any(b > c for (_, b), (c, _) in zip(loads, loads[1:])):
        raise ValueError('overlapping PT_LOAD memory ranges (including BSS) are unsupported')
    executable = info['executable']
    if len(executable) != 1:
        raise ValueError('automated lifting requires exactly one executable PT_LOAD')
    segment = executable[0]
    start, end = segment['addr'], segment['addr'] + segment['filesz']
    sections = [s for s in info['sections'] if s['flags'] & 4 and s['size']]
    ranges = sorted((s['addr'], s['addr'] + s['size']) for s in sections) or [(start, end)]
    if any(a < start or b > end for a, b in ranges):
        raise ValueError('executable sections outside supported text segment')
    if any(b > c for (_, b), (c, _) in zip(ranges, ranges[1:])):
        raise ValueError('overlapping executable sections are unsupported')
    if not any(a <= info['entry'] < b for a, b in ranges):
        raise ValueError('ELF entry is not in an executable file range')
    return ranges


def publish(stage, destination):
    """Replace owned output as a unit, never reuse a previous generated lift."""
    backup = destination.with_name(destination.name + '.previous')
    if backup.exists():
        raise ValueError('previous interrupted publication exists: ' + str(backup))
    if destination.exists():
        destination.rename(backup)
    try:
        stage.rename(destination)
    except BaseException:
        if backup.exists():
            backup.rename(destination)
        raise
    if backup.exists():
        shutil.rmtree(backup)


def build_programs(args, registry_writer):
    destination = Path(args.lifted).absolute()
    registry = Path(args.out).absolute()
    if reparse_path(destination):
        raise SystemExit('--input output directory must not be a symlink or reparse point')
    if registry.parent != destination:
        raise SystemExit('--input --out must be directly inside the owned --lifted directory')
    if destination.exists() and any(destination.iterdir()):
        try:
            previous = json.loads((destination / 'inventory.json').read_text())
            owned = previous.get('generator') == 'build_spu_workloads' and previous.get('schema_version') == 1
        except (OSError, ValueError):
            owned = False
        if not owned:
            raise SystemExit('--input requires an empty or previously generated output directory')
    inputs = sorted({str(Path(p).resolve()) for p in args.input})
    if any(Path(p).is_relative_to(destination.resolve()) for p in inputs):
        raise SystemExit('input files must be outside the owned output directory')
    # Failed, partial, or tampered generations always take the original fresh
    # staging path. A hit does not rewrite a single generated file or timestamp.
    try:
        cache_key = None if os.environ.get('PS3RECOMP_SPU_NO_CACHE') == '1' else generation_key(args, inputs, destination, registry)
    except OSError:
        # Preserve the original fresh path's failed inventory on missing input.
        cache_key = None
    if (cache_key is not None and not args.relift and not args.skip_lift
            and not destination.with_name(destination.name + '.previous').exists()
            and valid_cached_generation(destination, cache_key)):
        print('[build_spu_workloads] verified content cache hit; generated outputs unchanged')
        return 0
    destination.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=destination.name + '.stage-', dir=destination.parent))
    report = dict(schema_version=1, generator='build_spu_workloads', complete=False,
                  inputs=[], programs=[], errors=[], provenance={})
    try:
        if args.skip_lift:
            raise ValueError('--input always generates fresh code; --skip-lift is incompatible')
        if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', args.register_fn):
            raise ValueError('--register-fn must be a C identifier')
        # Hash every tool contributing to generation; fresh execution means changed
        # source/options/input can never validate an old registration implicitly.
        tool_names = ['spu_program_pipeline.py', 'build_spu_workloads.py', 'spu_elf.py',
                      'find_spu_functions.py', 'spu_disasm.py']
        tool_paths = [Path(__file__).with_name(n) for n in tool_names] + [Path(args.lifter).resolve()]
        tool_paths = generator_dependencies(args.lifter) or tool_paths
        report['provenance'] = dict(tools={str(p): sha256(p.read_bytes()) for p in tool_paths},
                                    python=sys.version, options=dict(register_fn=args.register_fn, constructor=args.constructor,
                                                 extra_funcs=args.extra_funcs, allow_partial=args.allow_partial))
        unique = {}
        blobs = {}
        layouts = {}
        for path in inputs:
            try:
                buf = Path(path).read_bytes()
            except OSError as error:
                report['errors'].append(str(error))
                continue
            report['inputs'].append(dict(path=path, sha256=sha256(buf), size=len(buf)))
            for offset, info, error in scan_spu_candidates(buf):
                occurrence = dict(path=path, offset=offset)
                if error:
                    report['programs'].append(dict(status='invalid', reason=error, occurrences=[occurrence]))
                    continue
                blob = buf[offset:offset + info['size']]
                digest = sha256(blob)
                if digest in unique:
                    if blobs[digest] != blob:
                        raise ValueError('SHA256 collision between distinct images')
                    unique[digest]['occurrences'].append(occurrence)
                    continue
                name = 'spu_' + digest
                image_id = 0x10000000 | int(digest[:7], 16)  # positive signed-int range, away from legacy IDs
                p = dict(name=name, sha256=digest, size=len(blob), entry=info['entry'], image_id=image_id,
                         fingerprint=f'{fnv1a64(blob):016X}', status='discovered', occurrences=[occurrence])
                unique[digest], blobs[digest], layouts[digest] = p, blob, info
                report['programs'].append(p)
        # Runtime keys cannot distinguish collisions, including identical text
        # with differing data or one image's text matching another's full ELF.
        owners = {}
        ids = {}
        for digest, program in sorted(unique.items()):
            info = layouts[digest]
            if len(info['executable']) == 1:
                x = info['executable'][0]
                program['text_fingerprint'] = f"{fnv1a64(blobs[digest][x['off']:x['off'] + x['filesz']]):016X}"
            keys = {program['fingerprint'], program.get('text_fingerprint')} - {None}
            for key in keys:
                owners.setdefault(key, []).append(program)
            ids.setdefault(program['image_id'], []).append(program)
        for group in list(owners.values()) + list(ids.values()):
            if len(group) > 1:
                for program in group:
                    program.update(status='unsupported', reason='ambiguous runtime fingerprint or image-id collision')
        extra = {}
        for spec in args.extra_funcs:
            name, sep, value = spec.partition('=')
            if not sep or name not in {p['name'] for p in unique.values()}:
                raise ValueError('--extra-funcs requires discovered content name=ADDR[,ADDR...]')
            extra[name] = [int(s.strip(), 0) for s in value.split(',')]
        imgs, text_fps, image_ids = [], {}, {}
        from find_spu_functions import detect_functions
        from build_spu_workloads import debase_includes
        for digest, program in sorted(unique.items()):
            if program['status'] == 'unsupported':
                continue
            name, prefix = program['name'], program['name'] + '_'
            outdir = stage / name
            outdir.mkdir()
            blob, info = blobs[digest], layouts[digest]
            (outdir / 'image.elf').write_bytes(blob)
            try:
                ranges = code_ranges(info)
                functions, (offset, base, size) = detect_functions(blob, verbose=False)
                bounds = sorted({(max(s, a), min(e, b)) for s, e in functions for a, b in ranges
                                 if max(s, a) < min(e, b)})
                for address in extra.get(name, []):
                    if not any(a <= address < b and address % 4 == 0 for a, b in ranges):
                        raise ValueError('extra function address is outside executable ranges')
                program['executable_ranges'] = ranges
                (outdir / 'code.bin').write_bytes(blob[offset:offset + size])
                (outdir / 'functions.json').write_text(json.dumps([dict(start=s, end=e) for s, e in bounds]))
                command = [sys.executable, args.lifter, str(outdir / 'code.bin'), '--base', str(base),
                           '--functions', str(outdir / 'functions.json'), '--symbol-prefix', prefix,
                           '--report', str(outdir / 'lift.json'), '--output', str(outdir)]
                if extra.get(name):
                    command += ['--extra-funcs', ','.join(hex(a) for a in extra[name])]
                result = subprocess.run(command, capture_output=True, text=True)
                (outdir / 'lift.log').write_text(result.stdout + result.stderr)
                if result.returncode:
                    raise ValueError('lifter failed; see ' + name + '/lift.log')
                diagnostics = json.loads((outdir / 'lift.json').read_text())
                program['diagnostics'] = diagnostics
                if diagnostics['unsupported']:
                    raise ValueError('unsupported instructions emitted by lifter')
                starts = {s for s, _ in diagnostics['functions']}
                if info['entry'] not in starts:
                    raise ValueError('lifter did not generate the ELF entry')
                if any(not any(a <= s < e <= b for a, b in ranges) for s, e in diagnostics['functions']):
                    raise ValueError('generated function extends beyond executable ranges')
                if (set(diagnostics['call_targets']) | set(diagnostics['branch_targets'])) - starts:
                    raise ValueError('direct branch/call targets lack generated function entries')
                for filename in ['spu_recomp.c', 'spu_recomp.h']:
                    if not (outdir / filename).is_file():
                        raise ValueError('lifter output missing ' + filename)
                    debase_includes(str(outdir / filename))
                expected = f"{prefix}spu_func_{info['entry']:08X}("
                if '/* TODO spu:' in (outdir / 'spu_recomp.c').read_text():
                    raise ValueError('unresolved instruction or branch emitted by lifter')
                if expected not in (outdir / 'spu_recomp.h').read_text():
                    raise ValueError('generated header is missing entry symbol')
            except (ValueError, OSError, KeyError, SystemExit) as error:
                program.update(status='unsupported', reason=str(error))
                continue
            program['status'] = 'registered'
            imgs.append((name, prefix, int(program['fingerprint'], 16), info['entry']))
            image_ids[name] = program['image_id']
            text_fps[name] = int(program['text_fingerprint'], 16)
        # Default runtime capacity is 256 registrations, including text aliases.
        if sum(len({fp, text_fps[name]}) for name, _, fp, _ in imgs) > 256:
            report['errors'].append('generated registrations exceed default runtime capacity (256)')
        if sum(len(p.get('diagnostics', {}).get('functions', [])) for p in report['programs']
               if p['status'] == 'registered') > 262144:
            report['errors'].append('generated functions exceed default runtime registry capacity (262144)')
        report['complete'] = not report['errors'] and all(p['status'] == 'registered' for p in report['programs'])
        usable = not report['errors'] and (report['complete'] or args.allow_partial)
        if usable:
            registry_args = type('RegistryArgs', (), dict(out=str(stage / registry.name), title='discovered SPU programs',
                                                        register_fn=args.register_fn, constructor=args.constructor))()
            registry_writer(registry_args, imgs, text_fps, image_ids)
            sources = [destination / name / 'spu_recomp.c' for name, _, _, _ in imgs] + [registry]
            # Bracket arguments preserve spaces and avoid expansion of $ in paths.
            cmake = 'set(SPU_GENERATED_SOURCES\n' + ''.join(f'  [==[{p.as_posix()}]==]\n' for p in sources) + ')\n'
            (stage / 'sources.cmake').write_text(cmake)
        if not usable:
            for program in report['programs']:
                if program['status'] == 'registered':
                    program.update(status='generated', reason='registration omitted because coverage is incomplete')
        for program in report['programs']:
            if 'name' in program:
                outdir = stage / program['name']
                if outdir.exists():
                    program['outputs'] = {p.relative_to(stage).as_posix(): sha256(p.read_bytes())
                                          for p in sorted(outdir.iterdir()) if p.is_file()}
        report['outputs'] = {filename: sha256((stage / filename).read_bytes())
                             for filename in [registry.name, 'sources.cmake'] if (stage / filename).is_file()}
        # Do not certify a snapshot if any input/tool changed while lifting.
        # Fresh generation and its diagnostics remain valid as before, but the
        # next configure must regenerate rather than reuse that mixed snapshot.
        if (report['complete'] and cache_key is not None
                and report['inputs'] == cache_key['inputs']
                and generation_key(args, inputs, destination, registry) == cache_key):
            report['cache_key'] = cache_key
        (stage / 'inventory.json').write_text(json.dumps(report, indent=2) + '\n')
        publish(stage, destination)
        print(f"[build_spu_workloads] {len(imgs) if usable else 0}/{len(report['programs'])} candidates registered; "
              f"complete={report['complete']}; report={destination / 'inventory.json'}")
        return 0 if usable else 1
    except (ValueError, OSError) as error:
        report['complete'] = False
        report['errors'].append(str(error))
        for filename in [registry.name, 'sources.cmake']:
            (stage / filename).unlink(missing_ok=True)
        (stage / 'inventory.json').write_text(json.dumps(report, indent=2) + '\n')
        publish(stage, destination)
        print('[build_spu_workloads] generation failed: ' + str(error), file=sys.stderr)
        return 1
    finally:
        if stage.exists():
            shutil.rmtree(stage)
