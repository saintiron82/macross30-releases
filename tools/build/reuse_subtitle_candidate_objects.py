"""Reuse proven objects from an isolated subtitle candidate in a fresh frozen plan.

This edits only the supplied new plan, atomically. It never compiles, links,
copies objects, changes a normal build output, or starts the game. Uncertain
objects stay in the new plan's compile list. --self-test uses independent mocks.
"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import tempfile
import time
import unittest

from build_subtitle_candidate import BASE, ROOT, validate_plan


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))


def key(path):
    return str(Path(path).resolve()).casefold()


def inside(path, parent):
    path = Path(path).resolve()
    if not path.is_relative_to(Path(parent).resolve()):
        raise ValueError('Path escaped its trusted directory: ' + str(path))
    return path


def normalized(command):
    """The only ignored compiler arguments are attached /Fo and /Fd outputs."""
    if not isinstance(command, list) or not all(isinstance(x, str) for x in command):
        raise ValueError('Malformed compiler command')
    if any(x.startswith('@') for x in command):
        raise ValueError('Opaque compiler response files are not reusable')
    if '/showincludes' not in [x.lower() for x in command]:
        raise ValueError('Previous compiler did not request dependency output')
    return [x for x in command if not x.lower().startswith(('/fo', '/fd'))]


def dependencies(log):
    found = {}
    for line in log.read_text(encoding='utf-8').splitlines():
        match = re.match(r'^\s*Note: including file:\s*(.*?)\s*$', line)
        if not match:
            continue
        raw = match.group(1)
        path = Path(raw)
        if not raw or '\x00' in raw or not path.is_absolute():
            raise ValueError('Unresolved include dependency in compiler log')
        path = path.resolve()
        if not path.is_file():
            raise ValueError('Missing include dependency: ' + str(path))
        found[key(path)] = path
    if not found:
        raise ValueError('No parseable include dependencies; cannot prove closure')
    return list(found.values())


def inherited_compile(previous, old, source, preserved, seen=()):
    """Resolve one explicitly inherited object; never infer ancestry from a path."""
    identity = key(previous)
    if identity in seen or len(seen) >= 16:
        raise ValueError('Candidate reuse ancestry cycle or depth limit')
    local = [r for r in old['compile'] if key(r['source']) == key(source)]
    if local:
        if len(local) != 1:
            raise ValueError('No unique previous compile record')
        return local[0], previous, old, {}
    metadata = old.get('candidate_object_reuse', {})
    proofs = [r for r in metadata.get('reused', []) if key(r['source']) == key(source)]
    if len(proofs) != 1:
        raise ValueError('No unique previous compile or inherited record')
    proof = proofs[0]
    parent = inside(metadata['previous'], BASE)
    if key(parent) in seen + (identity,):
        raise ValueError('Candidate reuse ancestry cycle or depth limit')
    parent_plan = parent / 'plan.json'
    parent_result = parent / 'build-result.json'
    ancestor = read_json(parent_plan)
    result = read_json(parent_result)
    validate_plan(ancestor)
    if Path(ancestor['output']).resolve() != parent:
        raise ValueError('Inherited plan output mismatch')
    if ancestor['preserved_original_outputs'] != preserved or result.get('original_outputs') != preserved:
        raise ValueError('Inherited candidate normal-build epoch differs')
    if result.get('status') != 'compiled_and_linked_candidate_only' or not all(
        result.get(k) is True for k in
        ('original_outputs_preserved', 'reused_objects_unchanged', 'frozen_sources_unchanged')):
        raise ValueError('Inherited candidate lacks preservation guards')
    exe = inside(ancestor['products']['exe'], parent)
    product = result.get('products', {}).get('exe', {})
    if (Path(product.get('path', '')).resolve() != exe or digest(exe) != product.get('sha256') or
            product.get('sha256') != metadata.get('prior_executable_sha256')):
        raise ValueError('Inherited executable product hash mismatch')
    if parent_result.stat().st_mtime_ns < parent_plan.stat().st_mtime_ns:
        raise ValueError('Inherited result predates plan')
    guards = {key(p): h for p, h in old['source_guards'].items()}
    pins = {}
    def pin(path, expected=None):
        path = Path(path).resolve()
        actual = digest(path)
        if guards.get(key(path)) != actual or (expected is not None and expected != actual):
            raise ValueError('Inherited frozen proof changed: ' + str(path))
        pins[str(path)] = actual
    for path in (parent_plan, parent_result, exe):
        pin(path)
    pin(source, proof['source_sha256'])
    obj = Path(proof['previous_object']).resolve()
    log = Path(proof['compile_log']).resolve()
    if old['response_objects'].count(str(obj)) != 1:
        raise ValueError('Inherited object not uniquely linked')
    if old['reused_object_stamps'].get(str(obj)) != [obj.stat().st_size, obj.stat().st_mtime_ns]:
        raise ValueError('Inherited object stamp changed')
    pin(obj, proof['object_sha256'])
    pin(log, proof['compile_log_sha256'])
    for dep in proof['dependencies']:
        pin(dep['path'], dep['sha256'])
    row, origin, origin_plan, older_pins = inherited_compile(
        parent, ancestor, source, preserved, seen + (identity,))
    if (key(row['output']) != key(obj) or row['source_sha256'] != proof['source_sha256'] or
            hashlib.sha256(json.dumps(normalized(row['command'])).encode()).hexdigest() !=
            proof['compiler_arguments_sha256']):
        raise ValueError('Inherited compile provenance differs')
    pins.update(older_pins)
    return row, origin, origin_plan, pins


def make_reuse_plan(previous, new_path):
    previous = inside(previous, BASE)
    new_path = inside(new_path, BASE)
    old_plan_path = previous / 'plan.json'
    result_path = previous / 'build-result.json'
    old = read_json(old_plan_path)
    result = read_json(result_path)
    new = read_json(new_path)
    validate_plan(old)
    validate_plan(new)
    if Path(old['output']).resolve() != previous:
        raise ValueError('Previous plan does not belong to the selected candidate')
    new_output = Path(new['output']).resolve()
    if new_path.parent != new_output or new_output == previous:
        raise ValueError('Require a plan in a different isolated output directory')
    if new.get('candidate_object_reuse'):
        raise ValueError('Reuse was already prepared for this plan')
    if Path(new['products']['exe']).exists() or (new_output / 'build-result.json').exists():
        raise ValueError('New candidate is already built')
    if result.get('status') != 'compiled_and_linked_candidate_only' or not all(
        result.get(k) is True for k in
        ('original_outputs_preserved', 'reused_objects_unchanged', 'frozen_sources_unchanged')
    ):
        raise ValueError('Previous build lacks successful preservation guards')
    preserved = old['preserved_original_outputs']
    if not preserved or result.get('original_outputs') != preserved or new['preserved_original_outputs'] != preserved:
        raise ValueError('Normal-build epoch differs between the two candidates')
    for name, expected in preserved.items():
        if not Path(name).is_file() or digest(name) != expected:
            raise ValueError('Normal-build output changed: ' + name)
    old_exe = inside(old['products']['exe'], previous)
    product = result.get('products', {}).get('exe', {})
    if Path(product.get('path', '')).resolve() != old_exe or not old_exe.is_file() or digest(old_exe) != product.get('sha256'):
        raise ValueError('Previous executable product hash mismatch')

    old_mtime = old_plan_path.stat().st_mtime_ns
    result_mtime = result_path.stat().st_mtime_ns
    if result_mtime < old_mtime:
        raise ValueError('Previous result predates its frozen plan')
    before = copy.deepcopy(new)
    old_guards = {key(p): h for p, h in old['source_guards'].items()}
    new_guards = {key(p): h for p, h in new['source_guards'].items()}
    if len({key(r['source']) for r in new['compile']}) != len(new['compile']):
        raise ValueError('Duplicate compile sources in the new plan')
    reused, skipped, keep = [], [], []
    pinned = {}
    hash_cache = {}

    def checked_hash(path):
        path = Path(path).resolve()
        st = path.stat()
        cache_key = (str(path), st.st_size, st.st_mtime_ns)
        if cache_key not in hash_cache:
            hash_cache[cache_key] = digest(path)
        return hash_cache[cache_key]

    for row in new['compile']:
        source = inside(row['source'], ROOT)
        try:
            prior, origin, origin_plan, ancestry_pins = inherited_compile(previous, old, source, preserved)
            origin_mtime = (origin / 'plan.json').stat().st_mtime_ns
            origin_result_mtime = (origin / 'build-result.json').stat().st_mtime_ns
            origin_guards = {key(p): h for p, h in origin_plan['source_guards'].items()}
            source_hash = checked_hash(source)
            if not (prior['source_sha256'] == row['source_sha256'] == source_hash ==
                    origin_guards.get(key(source)) == old_guards.get(key(source)) == new_guards.get(key(source))):
                raise ValueError('Source SHA differs')
            arguments = normalized(row['command'])
            if normalized(prior['command']) != arguments:
                raise ValueError('Compiler arguments differ beyond /Fo and /Fd')
            obj = inside(prior['output'], origin / 'objects')
            log = obj.with_suffix('.log')
            if not obj.is_file() or not log.is_file():
                raise ValueError('Previous compiled object or compile log is missing')
            if origin_plan['response_objects'].count(str(obj)) != 1:
                raise ValueError('Previous object was not uniquely linked')
            obj_stat, log_stat = obj.stat(), log.stat()
            if (obj_stat.st_mtime_ns < origin_mtime or log_stat.st_mtime_ns < origin_mtime or
                obj_stat.st_mtime_ns > origin_result_mtime or log_stat.st_mtime_ns > origin_result_mtime):
                raise ValueError('Object/log timestamps do not fit the previous build')
            listed = dependencies(log)
            compiler = Path(prior['command'][0]).resolve()
            if not compiler.is_file():
                raise ValueError('Compiler executable is unavailable')
            proof_deps = []
            candidate_pins = dict(ancestry_pins)
            # Pin the compiler too: an SDK/compiler update must not silently reuse
            # objects even if the command line retained the same executable path.
            for dep in dict.fromkeys(listed + [compiler]):
                dep_stat = dep.stat()
                if dep_stat.st_mtime_ns > origin_mtime:
                    raise ValueError('Dependency is newer than the previous plan: ' + str(dep))
                dep_hash = checked_hash(dep)
                historical = origin_guards.get(key(dep))
                if historical is not None and dep_hash != historical:
                    raise ValueError('Historically guarded dependency changed: ' + str(dep))
                current = new_guards.get(key(dep))
                if current is not None and dep_hash != current:
                    raise ValueError('New frozen dependency changed: ' + str(dep))
                candidate_pins[str(dep)] = dep_hash
                proof_deps.append(dict(path=str(dep), sha256=dep_hash,
                                       mtime_ns=dep_stat.st_mtime_ns,
                                       prior_guard_sha256=historical,
                                       workspace=dep.is_relative_to(ROOT)))
            if new['response_objects'].count(row['output']) != 1 or str(obj) in new['response_objects']:
                raise ValueError('New object replacement would be missing or duplicated')
            obj_hash = checked_hash(obj)
            candidate_pins[str(obj)] = obj_hash
            candidate_pins[str(log)] = checked_hash(log)
            for name, expected in candidate_pins.items():
                if name in pinned and pinned[name] != expected:
                    raise ValueError('A reuse dependency changed while preparing the plan')
            pinned.update(candidate_pins)
            new['response_objects'] = [str(obj) if p == row['output'] else p for p in new['response_objects']]
            new['reused_object_stamps'][str(obj)] = [obj_stat.st_size, obj_stat.st_mtime_ns]
            reused.append(dict(source=str(source), source_sha256=source_hash,
                               previous_object=str(obj), object_sha256=obj_hash,
                               compile_log=str(log), compile_log_sha256=candidate_pins[str(log)],
                               compiler_arguments_sha256=hashlib.sha256(json.dumps(arguments).encode()).hexdigest(),
                               previous_plan_mtime_ns=origin_mtime, origin_candidate=str(origin), dependencies=proof_deps))
        except (ValueError, OSError, UnicodeError, KeyError) as error:
            keep.append(row)
            skipped.append(dict(source=str(source), reason=str(error)))
    new['compile'] = keep
    new['source_compile_count'] = len(keep)
    new['reused_object_count'] = len(new['reused_object_stamps'])
    if reused:
        for path in (old_plan_path, result_path, old_exe):
            pinned[str(path)] = checked_hash(path)
        for name, expected in pinned.items():
            if name in new['source_guards'] and new['source_guards'][name] != expected:
                raise ValueError('Conflicting frozen guard: ' + name)
            new['source_guards'][name] = expected
    new['candidate_object_reuse'] = dict(
        previous=str(previous), reused_candidate_count=len(reused), reused=reused, skipped=skipped,
        dependency_history_basis='Previous /showIncludes closure; dependency/compiler mtime <= previous plan; previous guard SHA when available; current bytes pinned for execution',
        prior_executable_sha256=product['sha256'], original_outputs_unchanged=True)
    # Verify that the operation changed no linker outputs or other products.
    for field in ('output', 'link', 'products', 'response_file', 'response_libraries',
                  'preserved_original_outputs', 'additional_explicit_objects'):
        if new[field] != before[field]:
            raise ValueError('Reuse unexpectedly changed ' + field)
    validate_plan(new)
    return new


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--previous', type=Path)
    parser.add_argument('--plan', type=Path)
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(ReuseFixtures)
        result = unittest.TextTestRunner(verbosity=2).run(suite)
        raise SystemExit(0 if result.wasSuccessful() else 1)
    if args.previous is None or args.plan is None:
        parser.error('--previous and --plan are required')
    result = make_reuse_plan(args.previous, args.plan)
    path = args.plan.resolve()
    temporary = path.with_name(path.name + '.reuse.tmp')
    temporary.write_bytes((json.dumps(result, indent=2) + '\n').encode('utf-8'))
    os.replace(temporary, path)
    print(json.dumps(dict(reused_candidate_count=result['candidate_object_reuse']['reused_candidate_count'],
                          remaining_compile_count=len(result['compile']),
                          skipped=result['candidate_object_reuse']['skipped'])))


class ReuseFixtures(unittest.TestCase):
    """No production plans, compiler processes, objects or executables are used."""
    def setUp(self):
        BASE.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix='object-reuse-fixture-', dir=BASE)
        self.folder = Path(self.temp.name).resolve()
        assert self.folder.is_relative_to(BASE.resolve())
        self.addCleanup(self.temp.cleanup)
        self.old_dir, self.new_dir = self.folder/'old', self.folder/'new'
        for d in (self.old_dir/'objects', self.new_dir/'objects'):
            d.mkdir(parents=True)
        self.source, self.header, self.compiler = [self.folder/n for n in ('test.cpp','test.h','compiler.exe')]
        self.source.write_bytes(b'#include "test.h"\nint a;\n')
        self.header.write_bytes(b'#define TEST 1\n')
        self.compiler.write_bytes(b'mock compiler, never executed')
        self.base = self.folder/'normal.exe';self.base.write_bytes(b'normal output')
        self.at = time.time_ns()-5_000_000_000
        for p in (self.source,self.header,self.compiler,self.base):
            os.utime(p,ns=(self.at-1000,self.at-1000))
        self.old = self.plan(self.old_dir)
        self.new = self.plan(self.new_dir)
        self.object = Path(self.old['compile'][0]['output']);self.object.write_bytes(b'old compiled object')
        self.log = self.object.with_suffix('.log');self.log.write_text('Note: including file: '+str(self.header)+'\n',encoding='utf8')
        self.exe = Path(self.old['products']['exe']);self.exe.write_bytes(b'mock linked executable')
        self.result = dict(status='compiled_and_linked_candidate_only',original_outputs_preserved=True,
                           reused_objects_unchanged=True,frozen_sources_unchanged=True,
                           original_outputs=self.old['preserved_original_outputs'],
                           products=dict(exe=dict(path=str(self.exe),sha256=digest(self.exe))))
        self.write()

    def plan(self,out):
        output=out/'objects/test.obj'
        products={x:str(out/('Macross30Recompiled.'+x)) for x in ('exe','pdb','map','lib')}
        command=[str(self.compiler),'/showIncludes','/O1','/Fo'+str(output),'/Fd'+str(out/'objects/compile.pdb'),'-c','--',str(self.source)]
        return dict(status='prepared_not_built',output=str(out),products=products,
                    compile=[dict(source=str(self.source),source_sha256=digest(self.source),output=str(output),command=command)],
                    source_guards={str(self.source):digest(self.source),str(self.header):digest(self.header)},
                    preserved_original_outputs={str(self.base):digest(self.base)},reused_object_stamps={},
                    response_objects=[str(output)],response_libraries=['kernel32.lib'],response_file=str(out/'link.rsp'),
                    link=['link.exe']+['/'+flag+':'+products[k] for flag,k in [('out','exe'),('pdb','pdb'),('map','map'),('implib','lib')]],
                    additional_explicit_objects=[],source_compile_count=1,reused_object_count=0)

    def write(self):
        for p,d in ((self.old_dir/'plan.json',self.old),(self.new_dir/'plan.json',self.new),(self.old_dir/'build-result.json',self.result)):
            p.write_text(json.dumps(d),encoding='utf8')
        os.utime(self.old_dir/'plan.json',ns=(self.at,self.at))
        for p in (self.object,self.log,self.exe):os.utime(p,ns=(self.at+1000,self.at+1000))
        os.utime(self.old_dir/'build-result.json',ns=(self.at+2000,self.at+2000))

    def run_plan(self):return make_reuse_plan(self.old_dir,self.new_dir/'plan.json')
    def transitive(self):
        middle = self.run_plan()
        (self.new_dir/'plan.json').write_text(json.dumps(middle), encoding='utf8')
        os.utime(self.new_dir/'plan.json', ns=(self.at+4000,self.at+4000))
        exe = Path(middle['products']['exe']); exe.write_bytes(b'middle linked executable')
        result = copy.deepcopy(self.result)
        result['products']['exe'] = dict(path=str(exe),sha256=digest(exe))
        result_path = self.new_dir/'build-result.json'
        result_path.write_text(json.dumps(result),encoding='utf8')
        os.utime(result_path,ns=(self.at+6000,self.at+6000))
        latest_dir=self.folder/'latest';(latest_dir/'objects').mkdir(parents=True)
        latest=latest_dir/'plan.json';latest.write_text(json.dumps(self.plan(latest_dir)),encoding='utf8')
        return middle, latest

    def test_transitive_object_reused_with_ancestor_pins(self):
        _,latest=self.transitive()
        value=make_reuse_plan(self.new_dir,latest)
        self.assertEqual(value['compile'],[])
        self.assertEqual(value['response_objects'],[str(self.object)])
        for path in (self.old_dir/'plan.json',self.old_dir/'build-result.json',self.exe,
                     self.new_dir/'plan.json',self.new_dir/'build-result.json'):
            self.assertEqual(value['source_guards'][str(path)],digest(path))

    def test_two_hop_transitive_reuse(self):
        _,latest=self.transitive()
        inherited=make_reuse_plan(self.new_dir,latest)
        latest.write_text(json.dumps(inherited),encoding='utf8')
        os.utime(latest,ns=(self.at+8000,self.at+8000))
        exe=Path(inherited['products']['exe']);exe.write_bytes(b'third candidate')
        result=copy.deepcopy(self.result)
        result['products']['exe']=dict(path=str(exe),sha256=digest(exe))
        result_path=latest.parent/'build-result.json'
        result_path.write_text(json.dumps(result),encoding='utf8')
        os.utime(result_path,ns=(self.at+10000,self.at+10000))
        fourth=self.folder/'fourth';(fourth/'objects').mkdir(parents=True)
        final=fourth/'plan.json';final.write_text(json.dumps(self.plan(fourth)),encoding='utf8')
        value=make_reuse_plan(latest.parent,final)
        self.assertEqual(value['compile'],[])
        self.assertEqual(value['response_objects'],[str(self.object)])
        self.assertIn(str(self.old_dir/'plan.json'),value['source_guards'])
        self.assertIn(str(self.new_dir/'plan.json'),value['source_guards'])
        self.assertIn(str(latest),value['source_guards'])

    def test_transitive_changed_header_recompiles(self):
        _,latest=self.transitive()
        self.header.write_bytes(b'changed header')
        os.utime(self.header,ns=(self.at-1000,self.at-1000))
        self.assertEqual(len(make_reuse_plan(self.new_dir,latest)['compile']),1)

    def test_transitive_changed_ancestor_plan_recompiles(self):
        _,latest=self.transitive()
        plan=self.old_dir/'plan.json'
        plan.write_text(plan.read_text()+' ',encoding='utf8')
        self.assertEqual(len(make_reuse_plan(self.new_dir,latest)['compile']),1)

    def test_transitive_wrong_base_recompiles(self):
        _,latest=self.transitive()
        self.old['preserved_original_outputs'][str(self.base)]='0'*64
        (self.old_dir/'plan.json').write_text(json.dumps(self.old),encoding='utf8')
        value=make_reuse_plan(self.new_dir,latest)
        self.assertEqual(len(value['compile']),1)
        self.assertIn('epoch differs',value['candidate_object_reuse']['skipped'][0]['reason'])

    def test_transitive_cycle_recompiles(self):
        middle,latest=self.transitive()
        middle['candidate_object_reuse']['previous']=str(self.new_dir)
        (self.new_dir/'plan.json').write_text(json.dumps(middle),encoding='utf8')
        os.utime(self.new_dir/'plan.json',ns=(self.at+4000,self.at+4000))
        value=make_reuse_plan(self.new_dir,latest)
        self.assertEqual(len(value['compile']),1)
        self.assertIn('cycle',value['candidate_object_reuse']['skipped'][0]['reason'])

    def test_transitive_tampered_object_recompiles(self):
        _,latest=self.transitive()
        self.object.write_bytes(b'tampered same timestamp')
        os.utime(self.object,ns=(self.at+1000,self.at+1000))
        self.assertEqual(len(make_reuse_plan(self.new_dir,latest)['compile']),1)

    def test_transitive_untrusted_ancestor_exe_recompiles(self):
        _,latest=self.transitive()
        self.exe.write_bytes(b'tampered ancestor executable')
        self.assertEqual(len(make_reuse_plan(self.new_dir,latest)['compile']),1)

    def test_matching_object_is_reused_and_guards_pinned(self):
        value=self.run_plan();self.assertEqual(value['compile'],[])
        self.assertEqual(value['response_objects'],[str(self.object)])
        self.assertEqual(value['source_guards'][str(self.object)],digest(self.object))
        self.assertIn(str(self.header),value['source_guards'])
        self.assertEqual(value['link'],self.new['link'])
    def test_changed_source_recompiles(self):
        self.source.write_bytes(b'changed source');self.new['source_guards'][str(self.source)]=digest(self.source)
        self.new['compile'][0]['source_sha256']=digest(self.source);self.write()
        self.assertEqual(len(self.run_plan()['compile']),1)
    def test_newer_header_recompiles(self):
        os.utime(self.header,ns=(self.at+100,self.at+100))
        self.assertEqual(len(self.run_plan()['compile']),1)
    def test_changed_guarded_header_with_old_timestamp_recompiles(self):
        self.header.write_bytes(b'#define TEST 2\n');os.utime(self.header,ns=(self.at-1000,self.at-1000))
        self.assertEqual(len(self.run_plan()['compile']),1)
    def test_missing_dependency_recompiles(self):
        self.header.unlink();self.assertEqual(len(self.run_plan()['compile']),1)
    def test_missing_include_closure_recompiles(self):
        self.log.write_text('compiler emitted no dependencies\n',encoding='utf8');self.write()
        self.assertEqual(len(self.run_plan()['compile']),1)
    def test_compiler_flag_change_recompiles(self):
        self.new['compile'][0]['command'][2]='/O2';self.write()
        self.assertEqual(len(self.run_plan()['compile']),1)
    def test_untrusted_previous_executable_rejected(self):
        self.exe.write_bytes(b'tampered executable')
        with self.assertRaisesRegex(ValueError,'executable product hash'):self.run_plan()
    def test_normal_output_epoch_change_rejected(self):
        self.new['preserved_original_outputs'][str(self.base)]='0'*64;self.write()
        with self.assertRaisesRegex(ValueError,'epoch differs'):self.run_plan()
    def test_failed_previous_guard_rejected(self):
        self.result['reused_objects_unchanged']=False;self.write()
        with self.assertRaisesRegex(ValueError,'preservation guards'):self.run_plan()
    def test_object_modified_after_build_recompiles(self):
        os.utime(self.object,ns=(self.at+3000,self.at+3000))
        self.assertEqual(len(self.run_plan()['compile']),1)


if __name__ == '__main__':
    main()
