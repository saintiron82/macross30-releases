"""Content-cache integration fixtures. No guest assets, compiler or native runs."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from test_spu_program_pipeline import elf

TOOLS = Path(__file__).resolve().parent
HIT = 'verified content cache hit'


class ContentCache(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='spu-cache-fixture-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.tools = self.root / 'tools'
        self.tools.mkdir()
        for source in TOOLS.glob('*.py'):
            shutil.copyfile(source, self.tools / source.name)
        self.source = self.root / 'tiny.elf'
        self.source.write_bytes(elf(7))
        self.output = self.root / 'generated'
        self.environment = dict(os.environ, MACROSS_SPU_STATIC_ENTRIES='0',
                                PYTHONDONTWRITEBYTECODE='1', PS3RECOMP_SPU_NO_CACHE='0')
        self.assert_fresh(self.run_pipeline())

    def run_pipeline(self, *extra, environment=None):
        return subprocess.run(
            [sys.executable, str(self.tools / 'build_spu_workloads.py'),
             '--input', str(self.source), '--lifted', str(self.output),
             '--out', str(self.output / 'spu_workloads.c'), *extra],
            env=environment or self.environment, capture_output=True,
            text=True, timeout=30)

    def assert_fresh(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn(HIT, result.stdout)
        self.assertTrue(self.manifest()['complete'])

    def manifest(self):
        return json.loads((self.output / 'inventory.json').read_text())

    def snapshot(self):
        return {str(p.relative_to(self.output)):
                (p.stat().st_mtime_ns, p.stat().st_size,
                 hashlib.sha256(p.read_bytes()).hexdigest() if p.is_file() else None)
                for p in [self.output, *self.output.rglob('*')]}

    def test_verified_hit_changes_no_file_or_directory_mtime(self):
        before = self.snapshot()
        result = self.run_pipeline()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(HIT, result.stdout)
        self.assertEqual(before, self.snapshot())

    def test_input_bytes_invalidate_even_with_size_and_mtime_unchanged(self):
        stamp = self.source.stat()
        self.source.write_bytes(elf(9))
        os.utime(self.source, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
        self.assert_fresh(self.run_pipeline())

    def test_input_mtime_alone_does_not_invalidate(self):
        before = self.snapshot()
        os.utime(self.source, ns=(1_000_000_000, 1_000_000_000))
        self.assertIn(HIT, self.run_pipeline().stdout)
        self.assertEqual(before, self.snapshot())

    def test_real_lifter_source_bytes_invalidate(self):
        lifter = self.tools / 'spu_lifter.py'
        stamp = lifter.stat()
        lifter.write_bytes(lifter.read_bytes() + b'\n# cache test dependency change\n')
        os.utime(lifter, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
        self.assert_fresh(self.run_pipeline())

    def test_environment_change_invalidates(self):
        env = dict(self.environment, MACROSS_SPU_STATIC_ENTRIES='1')
        self.assert_fresh(self.run_pipeline(environment=env))

    def test_generation_options_change_invalidates(self):
        self.assert_fresh(self.run_pipeline('--constructor'))

    def test_output_bytes_tamper_invalidates_even_with_mtime_unchanged(self):
        generated = next(self.output.glob('spu_*/spu_recomp.c'))
        stamp = generated.stat()
        generated.write_bytes(generated.read_bytes() + b'\n/* tamper */\n')
        os.utime(generated, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
        self.assert_fresh(self.run_pipeline())
        self.assertNotIn(b'tamper', generated.read_bytes())

    def test_incomplete_manifest_cannot_hit(self):
        path = self.output / 'inventory.json'
        manifest = self.manifest()
        manifest['complete'] = False
        path.write_text(json.dumps(manifest))
        self.assert_fresh(self.run_pipeline())

    def test_missing_output_cannot_hit(self):
        next(self.output.glob('spu_*/lift.json')).unlink()
        self.assert_fresh(self.run_pipeline())

    def test_unexpected_file_cannot_hit(self):
        (self.output / 'unexpected.txt').write_text('unowned')
        self.assert_fresh(self.run_pipeline())
        self.assertFalse((self.output / 'unexpected.txt').exists())

    def test_relift_explicitly_bypasses_cache(self):
        self.assert_fresh(self.run_pipeline('--relift'))

    def test_environment_disable_uses_fresh_path(self):
        env = dict(self.environment, PS3RECOMP_SPU_NO_CACHE='1')
        self.assert_fresh(self.run_pipeline(environment=env))
        self.assertNotIn('cache_key', self.manifest())

    def test_environment_disable_missing_input_publishes_failure(self):
        self.source.unlink()
        env = dict(self.environment, PS3RECOMP_SPU_NO_CACHE='1')
        result = self.run_pipeline(environment=env)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn(HIT, result.stdout)
        self.assertFalse(self.manifest()['complete'])
        self.assertTrue(self.manifest()['errors'])

    def test_missing_input_produces_failed_inventory_not_old_complete_result(self):
        self.source.unlink()
        result = self.run_pipeline()
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn(HIT, result.stdout)
        self.assertFalse(self.manifest()['complete'])
        self.assertTrue(self.manifest()['errors'])

    def test_output_file_symlink_cannot_hit(self):
        target = self.output / 'sources.cmake'
        outside = self.root / 'outside.cmake'
        outside.write_bytes(target.read_bytes())
        target.unlink()
        try:
            target.symlink_to(outside)
        except OSError as error:
            self.skipTest(f'file symlinks unavailable: {error}')
        self.assertNotIn(HIT, self.run_pipeline().stdout)

    def junction(self, path, target):
        if os.name != 'nt':
            path.symlink_to(target, target_is_directory=True)
            self.addCleanup(lambda: path.unlink() if path.is_symlink() else None)
            return
        result = subprocess.run(['cmd', '/c', 'mklink', '/J', str(path), str(target)],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.addCleanup(lambda: os.rmdir(path) if path.exists() and path.is_junction() else None)

    def test_destination_directory_junction_cannot_hit(self):
        outside = self.root / 'outside-generated'
        self.output.rename(outside)
        self.junction(self.output, outside)
        before = hashlib.sha256((outside / 'inventory.json').read_bytes()).hexdigest()
        result = self.run_pipeline()
        self.assertNotIn(HIT, result.stdout)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertEqual(before, hashlib.sha256((outside / 'inventory.json').read_bytes()).hexdigest())

    def test_nested_empty_directory_junction_cannot_hit(self):
        outside = self.root / 'outside-empty'
        outside.mkdir()
        self.junction(self.output / 'unexpected-link', outside)
        self.assertNotIn(HIT, self.run_pipeline().stdout)

    def test_destination_parent_junction_cannot_hit(self):
        container = self.root / 'container'
        container.mkdir()
        self.output.rename(container / 'generated')
        self.output = container / 'generated'
        # Generate once at the final lexical path before redirecting its parent.
        self.assert_fresh(self.run_pipeline())
        outside = self.root / 'outside-container'
        container.rename(outside)
        self.junction(container, outside)
        self.assertNotIn(HIT, self.run_pipeline().stdout)


if __name__ == '__main__':
    unittest.main()
