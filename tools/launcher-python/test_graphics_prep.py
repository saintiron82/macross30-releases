"""Launch-time graphics preparation: fake-tool unit tests plus an optional
end-to-end run of the real MacrossShaderPrep.exe through Backend.prepare_graphics.

The end-to-end test needs a built candidate (bin layout is assembled in a temp
folder): set MACROSS_SHADER_PREP_CANDIDATE to a directory holding
MacrossShaderPrep.exe and assets/ (rsx_shader_cache_v2 + rsx_pso_inventory_v1.bin).
"""
import io
import json
import os
import shutil
import tempfile
import unittest
from pathlib import Path

import graphics_prep
from backend import Backend


class FakeProcess:
    def __init__(self, lines, code, on_start=None):
        self.stdout = io.StringIO(''.join(json.dumps(x) + '\n' for x in lines))
        self.code = code
        if on_start:
            on_start()

    def wait(self):
        return self.code


class FakeTool:
    """Scripted MacrossShaderPrep: --status answers, then one preparation run."""
    def __init__(self, needed=True, code=0, total=4):
        self.needed, self.code, self.total, self.calls = needed, code, total, []

    def __call__(self, command, **kwargs):
        self.calls.append(command)
        if '--status' in command:
            return FakeProcess([{'event': 'status', 'needed': int(self.needed)}], 10 if self.needed else 0)
        events = [{'event': 'progress', 'total': self.total, 'processed': i} for i in range(self.total + 1)]
        events.append({'event': 'done', 'total': self.total, 'processed': self.total, 'prepared': self.total,
                       'created': self.total, 'already': 0, 'missing_shader': 0, 'failed': 0})
        return FakeProcess(events, self.code)


class PreparationTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def tool(self):
        (self.root / 'bin').mkdir(exist_ok=True)
        (self.root / graphics_prep.TOOL).write_bytes(b'')

    def test_absent_tool_is_a_no_op(self):
        self.assertEqual(graphics_prep.Preparation(self.root).prepare()['status'], 'absent')

    def test_cache_follows_installed_launch_script(self):
        # prebuilt_finalize writes this line; the game then reads/writes its
        # shader cache, observed PSOs and pipeline library there, not in play/.
        local = self.root / 'local'
        (self.root / 'Macross30.cmd').write_text(
            '@echo off\r\nset "PS3_HDD0_ROOT=%_M30_ROOT%\\data\\hdd0"\r\n'
            'set "RSX_SHADER_CACHE_DIR=%LOCALAPPDATA%\\Macross30\\shader-cache-v2"\r\n'
            'if not exist "%LOCALAPPDATA%\\Macross30\\shader-cache-v2" mkdir "x"\r\n', encoding='utf-8')
        tool, cache = graphics_prep.paths(self.root, {'LOCALAPPDATA': str(local)})
        self.assertEqual(tool, self.root / graphics_prep.TOOL)
        self.assertEqual(cache, local / 'Macross30' / 'shader-cache-v2')
        # Unknown variable or relative result: renderer default under play/.
        self.assertEqual(graphics_prep.cache_dir(self.root, {}), self.root / graphics_prep.CACHE)
        (self.root / 'Macross30.cmd').write_text('set "RSX_SHADER_CACHE_DIR=%_M30_ROOT%\\cache"\r\n', encoding='utf-8')
        self.assertEqual(graphics_prep.cache_dir(self.root, {}), self.root / 'cache')
        (self.root / 'Macross30.cmd').write_text('set "RSX_SHADER_CACHE_DIR=relative\\cache"\r\n', encoding='utf-8')
        self.assertEqual(graphics_prep.cache_dir(self.root, {}), self.root / graphics_prep.CACHE)

    def test_no_launch_script_uses_renderer_default(self):
        self.assertEqual(graphics_prep.cache_dir(self.root), self.root / graphics_prep.CACHE)

    def test_current_library_only_checks_status(self):
        self.tool()
        fake = FakeTool(needed=False)
        result = graphics_prep.Preparation(self.root, popen=fake).prepare()
        self.assertEqual(result['status'], 'current')
        self.assertEqual(len(fake.calls), 1)
        self.assertIn('--status', fake.calls[0])
        self.assertIn(str(self.root / graphics_prep.CACHE), fake.calls[0])

    def test_needed_runs_bounded_cancellable_preparation_with_progress(self):
        self.tool()
        fake, seen = FakeTool(), []
        result = graphics_prep.Preparation(self.root, popen=fake).prepare(lambda d, t: seen.append((d, t)))
        self.assertEqual(result['status'], 'complete')
        run = fake.calls[1]
        self.assertIn('--cancel-file', run)
        self.assertIn('--max-seconds', run)
        self.assertEqual(seen[-1], (4, 4))

    def test_tool_exit_codes(self):
        self.tool()
        for code, status in ((2, 'cancelled'), (3, 'unsupported'), (1, 'error')):
            result = graphics_prep.Preparation(self.root, popen=FakeTool(code=code)).prepare()
            self.assertEqual(result['status'], status)

    def test_launch_failure_of_tool_never_raises(self):
        self.tool()
        def broken(*a, **k):
            raise OSError('cannot start')
        self.assertEqual(graphics_prep.Preparation(self.root, popen=broken).prepare()['status'], 'error')

    def test_backend_cancel_aborts_launch(self):
        self.tool()
        events = []
        backend = Backend('bridge.exe', self.root / 'launcher-settings.json', emit=events.append)
        backend.config['folder'] = str(self.root)
        cancelled = []

        def popen(command, **kwargs):
            if '--status' in command:
                return FakeProcess([{'event': 'status', 'needed': 1}], 10)
            # Cancel arrives while preparation runs (launcher window closed).
            return FakeProcess([{'event': 'done', 'total': 2, 'processed': 1}], 2,
                               on_start=lambda: cancelled.append(backend.cancel_graphics_prep()))
        original = graphics_prep.subprocess.Popen
        try:
            import backend as backend_module
            backend_module.external_popen, saved = popen, backend_module.external_popen
            result = backend.prepare_graphics()
        finally:
            backend_module.external_popen = saved
        self.assertEqual(cancelled, [True])
        self.assertTrue(backend.launch_aborted)
        self.assertEqual(result['status'], 'cancelled')
        self.assertTrue(any(e.get('type') == 'graphics-prep' for e in events))
        self.assertFalse(backend.cancel_graphics_prep())
        self.assertIs(graphics_prep.subprocess.Popen, original)


CANDIDATE = os.environ.get('MACROSS_SHADER_PREP_CANDIDATE')


@unittest.skipUnless(CANDIDATE and Path(CANDIDATE, 'MacrossShaderPrep.exe').is_file(), 'needs a built candidate')
class RealToolTests(unittest.TestCase):
    """Backend.prepare_graphics with the real tool, a real GPU and the shipped inventory."""
    def test_first_run_cancel_resume_and_current(self):
        source = Path(CANDIDATE)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'bin').mkdir()
            shutil.copy2(source / 'MacrossShaderPrep.exe', root / 'bin')
            shutil.copytree(source / 'assets/rsx_shader_cache_v2', root / 'bin/assets/rsx_shader_cache_v2')
            shutil.copy2(source / 'assets/rsx_pso_inventory_v1.bin', root / 'bin/assets')
            events = []
            backend = Backend('bridge.exe', root / 'launcher-settings.json', emit=events.append)
            backend.config['folder'] = str(root)
            prep = graphics_prep.Preparation(root)
            self.assertTrue(prep.needed())
            # First launch: cancel after the first progress report.
            def cancel_soon(done, total):
                if done >= 1:
                    prep.cancel()
            first = prep.prepare(cancel_soon)
            self.assertEqual(first['status'], 'cancelled')
            self.assertTrue(prep.needed())
            # Next launch resumes through the launcher path and completes.
            second = backend.prepare_graphics()
            self.assertEqual(second['status'], 'complete', second)
            self.assertGreater(second['already'], 0)
            self.assertEqual(second['prepared'], second['total'])
            self.assertFalse(backend.launch_aborted)
            self.assertTrue(any(e.get('type') == 'transfer' and e.get('unit') == 'items' for e in events))
            status = json.loads((root / graphics_prep.CACHE / 'pso-library-v1.json').read_text())
            self.assertEqual(status['status'], 'complete')
            self.assertFalse(status['driver_blob_portable'])
            # Third launch: nothing to do.
            self.assertEqual(backend.prepare_graphics()['status'], 'current')


if __name__ == '__main__':
    unittest.main()
