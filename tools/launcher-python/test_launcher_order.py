"""Exercise real UI entry points without creating a Tk window."""
from pathlib import Path
import queue
import tempfile
from types import MethodType, SimpleNamespace
import unittest
from unittest.mock import patch

from app import Launcher
from backend import Backend


class LauncherOrderTests(unittest.TestCase):
    def fixture(self):
        temp = tempfile.TemporaryDirectory(); self.addCleanup(temp.cleanup)
        backend = Backend('unused', Path(temp.name)/'settings.json')
        calls = []
        def current(*args, automatic=False):
            self.assertTrue(automatic)
            calls.append('launcher')
        backend.self_update = current
        backend.check = lambda: calls.append('game-metadata')
        backend.play = lambda: calls.append('game-install-or-play')
        ui = SimpleNamespace(backend=backend, events=queue.Queue(), preview=False,
                             args=SimpleNamespace(skip_launcher_update=False, update_health='health.txt'),
                             run=lambda action: action())
        ui.after_launcher = MethodType(Launcher.after_launcher, ui)
        return ui, calls

    @patch('sys.frozen', True, create=True)
    def test_restart_health_mode_still_checks_launcher_before_game_metadata(self):
        ui, calls = self.fixture()
        Launcher.startup(ui)
        self.assertEqual(calls, ['launcher', 'game-metadata'])

    @patch('sys.frozen', True, create=True)
    def test_play_checks_launcher_first_and_stops_for_handoff(self):
        ui, calls = self.fixture()
        Launcher.play_clicked(ui)
        self.assertEqual(calls, ['launcher', 'game-install-or-play'])
        calls.clear()
        ui.backend.self_update = lambda *a, **k: Path('update-work')
        Launcher.play_clicked(ui)
        self.assertEqual(calls, [])
        self.assertEqual(ui.events.get_nowait(), {'type': 'handoff'})

    @patch('sys.frozen', True, create=True)
    def test_startup_and_play_stop_on_launcher_failure(self):
        ui, calls = self.fixture()
        def failure(*a, **k):
            raise OSError('launcher update offline')
        ui.backend.self_update = failure
        for action in (Launcher.startup, Launcher.play_clicked):
            with self.assertRaises(OSError): action(ui)
        self.assertEqual(calls, [])


if __name__ == '__main__':
    unittest.main()
