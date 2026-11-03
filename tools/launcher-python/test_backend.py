import copy
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from backend import Backend, localized, update_plan

STATUS = json.loads((Path(__file__).parents[1] / 'dist/content/STATUS.json').read_text(encoding='utf-8'))


class BackendTests(unittest.TestCase):
    def test_launcher_first_runs_game_work_only_when_launcher_is_current(self):
        backend, _, calls = self.fixture()
        def current(executable, automatic=False):
            self.assertTrue(automatic)
            calls.append('launcher-current')
        backend.self_update = current
        self.assertIsNone(backend.launcher_first('launcher.exe', lambda: calls.append('game-work')))
        self.assertEqual(calls, ['launcher-current', 'game-work'])

    def test_launcher_handoff_prevents_game_work_in_old_process(self):
        backend, _, calls = self.fixture()
        backend.self_update = lambda *a, **k: Path('pending-update')
        self.assertEqual(backend.launcher_first('launcher.exe', lambda: calls.append('game-work')), Path('pending-update'))
        self.assertEqual(calls, [])

    def test_failed_launcher_check_or_download_prevents_game_work_and_can_retry(self):
        backend, _, calls = self.fixture()
        for error in (OSError('metadata offline'), ValueError('download hash mismatch')):
            backend.self_update = lambda *a, **k: (_ for _ in ()).throw(error)
            with self.assertRaises(type(error)):
                backend.launcher_first('launcher.exe', lambda: calls.append('game-work'))
        self.assertEqual(calls, [])
        backend.self_update = lambda *a, **k: None
        backend.launcher_first('launcher.exe', lambda: calls.append('game-work'))
        self.assertEqual(calls, ['game-work'])

    def test_automatic_launcher_update_does_not_prompt_and_preserves_config(self):
        with tempfile.TemporaryDirectory() as td:
            folder = Path(td)
            current, downloaded, helper = (folder/n for n in ('Macross30Launcher.exe','download.exe','bridge.exe'))
            current.write_bytes(b'old'); downloaded.write_bytes(b'new'); helper.write_bytes(b'helper')
            def no_prompt(_):
                self.fail('Automatic launcher updates must not prompt')
            backend = Backend(helper, folder/'custom-settings.json', confirm=no_prompt)
            backend.json = lambda _: dict(version='999.0.0',file='Macross30Launcher-999.0.0.exe',sha256='a'*64,bytes=3)
            backend.download = lambda *a, **k: downloaded
            backend.bridge = lambda *a, **k: {'started':123}
            with patch('backend.external_popen') as start:
                work = backend.self_update(current, automatic=True)
            plan = json.loads((work/'plan.json').read_text())
            self.assertEqual(plan['config'], str(backend.config_path.resolve()))
            self.assertEqual(current.read_bytes(), b'old')
            self.assertEqual((work/'new.exe').read_bytes(), b'new')
            start.assert_called_once()

    def test_launcher_update_failure_does_not_replace_current_exe(self):
        with tempfile.TemporaryDirectory() as td:
            current = Path(td)/'Macross30Launcher.exe';current.write_bytes(b'old')
            backend = Backend('unused', Path(td)/'settings.json')
            backend.json = lambda _: dict(version='999.0.0',file='Macross30Launcher-999.0.0.exe')
            backend.download = lambda *a, **k: (_ for _ in ()).throw(OSError('offline'))
            with self.assertRaises(OSError):backend.self_update(current, automatic=True)
            self.assertEqual(current.read_bytes(), b'old')
            self.assertFalse(any(Path(td).glob('.launcher-update-*')))

    def fixture(self):
        temporary = tempfile.TemporaryDirectory(prefix='macross-python-test-')
        self.addCleanup(temporary.cleanup)
        calls = []
        backend = Backend('unused', Path(temporary.name) / 'settings.json',
                          confirm=lambda data: calls.append('approve') or True)
        backend.config.update(folder=temporary.name, game_language='en-US')
        backend.manifest = {'version': '0.3.1', 'files': {'program': {'file': 'game'}},
                            'language_packs': {'en-US': {'file': 'Macross30-language-en-US-0.3.1.zip'}}}
        backend.status, backend.stale = copy.deepcopy(STATUS), False
        installed = dict(runtime_status=1, language_status=0, core_ready=True, language_ready=False, program='fixture')
        def inspect():
            backend.installed = copy.deepcopy(installed)
            return backend.installed
        def bridge(op, **extra):
            calls.append(op)
            if op == 'stage':
                installed['runtime_status'] = 2
            if op == 'launch':
                return {'exit_code': 0}
            return {'path': 'fixture'}
        def installer(*args, **kwargs):
            calls.append('language-install')
            installed.update(language_status=2, language_ready=True)
        backend.inspect, backend.bridge, backend.installer = inspect, bridge, installer
        backend.check_status = lambda: None
        backend.download = lambda row: calls.append('download:'+row['file']) or 'fixture.zip'
        return backend, installed, calls

    def test_independent_versions(self):
        self.assertEqual(update_plan(dict(runtime_status=2, language_status=0), 'en-US'), dict(core=False, language=True))
        self.assertEqual(update_plan(dict(runtime_status=1, language_status=2), 'ja-JP'), dict(core=True, language=False))

    def test_one_approval_update_language_launch(self):
        backend, installed, calls = self.fixture()
        backend.play()
        self.assertEqual(calls.count('approve'), 1)
        self.assertEqual(calls.count('launch'), 1)
        self.assertLess(calls.index('stage'), calls.index('language-install'))
        self.assertLess(calls.index('language-install'), calls.index('launch'))
        self.assertFalse(backend.busy)

    def test_decline_and_closed(self):
        backend, installed, calls = self.fixture()
        backend.confirm = lambda data: False
        backend.play()
        self.assertNotIn('launch', calls)
        self.assertFalse(any(op.startswith('download') for op in calls))
        backend.status['closed'] = True
        with self.assertRaises(RuntimeError):
            backend.play()
        self.assertFalse(any(op.startswith('download') for op in calls))

    def test_current_install_plays_without_download(self):
        backend, installed, calls = self.fixture()
        installed.update(runtime_status=2, language_status=2, language_ready=True)
        backend.play()
        self.assertIn('launch', calls)
        self.assertNotIn('approve', calls)

    def test_failed_install_allows_path_change_and_retry(self):
        backend, installed, calls = self.fixture()
        installed.update(core_ready=False, language_ready=False)
        backend.config['iso'] = __file__
        backend.inspect_iso = lambda: dict(kind='ready')
        backend.installed = installed.copy()
        backend.manifest['files']['program'] = dict(file='program')
        # Supply an already staged program, then fail original-data preparation.
        installed['program'] = 'fixture'
        def fail(*args, **kwargs):
            raise RuntimeError('Original preparation failed')
        backend.installer = fail
        with self.assertRaisesRegex(RuntimeError, 'Original preparation failed'):
            backend.play()
        self.assertFalse(backend.busy)
        self.assertFalse(backend.running)
        replacement = str(Path(backend.config['folder']) / 'retry-location')
        backend.settings(folder=replacement, iso='replacement.iso')
        self.assertEqual(backend.config['folder'], replacement)
        self.assertEqual(backend.config['iso'], 'replacement.iso')

    def test_selection_only_changes_settings(self):
        backend, installed, calls = self.fixture()
        backend.settings(game_language='ko-KR')
        self.assertEqual(backend.config['game_language'], 'ko-KR')
        self.assertEqual(calls, [])

    def test_localization_fallback(self):
        Backend.validate_status(STATUS)
        self.assertEqual(localized(STATUS['announcements'][0]['title'], 'ja'), STATUS['announcements'][0]['title']['ja'])
        self.assertEqual(localized({'ko': '기본'}, 'en'), '기본')

    def test_corrupt_download_preserves_install(self):
        temporary = tempfile.TemporaryDirectory(prefix='macross-hash-test-')
        self.addCleanup(temporary.cleanup)
        backend = Backend('unused', Path(temporary.name)/'settings.json', opener=lambda *args, **kwargs: io.BytesIO(b'bad'))
        backend.config['folder'] = temporary.name
        original = Path(temporary.name)/'release-profile.json'
        original.write_text('unchanged')
        row = dict(file='Macross30-program-0.3.1.zip', bytes=3, sha256='a'*64)
        with self.assertRaisesRegex(ValueError, 'SHA-256'):
            backend.download(row)
        self.assertEqual(original.read_text(), 'unchanged')
        self.assertEqual(list((Path(temporary.name)/'.local-build/downloads').iterdir()), [])


if __name__ == '__main__':
    unittest.main()
