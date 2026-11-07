"""Exercise launcher callbacks without creating or controlling any UI/window."""
import queue
import tempfile
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from app import Launcher
from backend import Backend


class ImmediateThread:
    def __init__(self, target, **kwargs):
        self.target = target

    def start(self):
        self.target()


class RuntimeOptionsTests(unittest.TestCase):
    def fixture(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        backend = Backend('unused', Path(temp.name) / 'config.json')
        backend.running = True
        return backend

    def test_running_game_uses_bridge_without_writing_settings(self):
        backend = self.fixture()
        calls = []
        backend.bridge = lambda op: calls.append(op) or {'requested': True}
        self.assertEqual(backend.open_game_options(), {'requested': True})
        self.assertEqual(calls, ['game-options'])
        self.assertFalse(backend.config_path.exists())
        self.assertTrue(backend.running)

    def test_install_operation_cannot_open_options(self):
        backend = self.fixture()
        backend.running, backend.busy = False, True
        with patch.object(backend, 'bridge') as bridge:
            with self.assertRaises(RuntimeError):
                backend.open_game_options()
            bridge.assert_not_called()

    def test_game_closed_or_unresponsive_is_reported_in_user_language(self):
        backend = self.fixture()
        for language in ('ko', 'en', 'ja'):
            backend.config['ui_language'] = language
            with patch.object(backend, 'bridge', side_effect=RuntimeError('game_options_no_game')):
                with self.assertRaises(RuntimeError) as caught:
                    backend.open_game_options()
                self.assertNotIn('game_options_', str(caught.exception))

    @patch('app.threading.Thread', ImmediateThread)
    def test_options_remain_available_during_blocking_launch_worker(self):
        backend = self.fixture()
        calls = []
        backend.bridge = lambda op: calls.append(op) or {'requested': True}
        ui = SimpleNamespace(state={'running': True}, worker_active=True,
                             preview=False, backend=backend, events=queue.Queue())
        Launcher.options_clicked(ui)
        self.assertEqual(calls, ['game-options'])
        self.assertTrue(ui.worker_active)
        events = [ui.events.get_nowait()['type'] for _ in range(ui.events.qsize())]
        self.assertEqual(events, ['runtime-options-opened', 'runtime-options-finished'])
        self.assertNotIn('worker-done', events)

    def test_preview_and_duplicate_request_do_not_control_game(self):
        backend = self.fixture()
        for preview, pending in ((True, False), (False, True)):
            ui = SimpleNamespace(state={'running': True}, preview=preview,
                                 runtime_options_pending=pending, backend=backend)
            with patch.object(backend, 'bridge') as bridge:
                Launcher.options_clicked(ui)
                bridge.assert_not_called()


if __name__ == '__main__':
    unittest.main()
