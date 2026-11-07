import unittest
from types import SimpleNamespace
from unittest.mock import Mock
from app import Launcher

class WorkerRecoveryTests(unittest.TestCase):
    def test_event_pump_reschedules_after_render_failure(self):
        ui=SimpleNamespace(drain_events=Mock(side_effect=RuntimeError('draw failed')),report_callback_exception=Mock(),winfo_exists=lambda:True,after=Mock(),drain=Mock())
        Launcher.drain(ui)
        ui.after.assert_called_once_with(40,ui.drain)
        ui.report_callback_exception.assert_called_once()

    def test_readonly_check_allows_close(self):
        ui=SimpleNamespace(backend=SimpleNamespace(cancel_graphics_prep=lambda:False),handoff=False,state={'running':False,'busy':False},worker_active=True,readonly_worker=True,destroy=Mock())
        Launcher.close(ui)
        ui.destroy.assert_called_once()
