import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from PIL import Image
from bundle_runtime import recover_bundle, load_hero


class BundleRuntimeTests(unittest.TestCase):
    def test_legacy_extraction_restarts_independently_with_original_arguments(self):
        with tempfile.TemporaryDirectory() as td, patch('sys.frozen', True, create=True), \
                patch('sys.argv', ['app.exe', '--config', 'settings with spaces.json']), \
                patch('bundle_runtime.subprocess.Popen') as start, \
                patch.dict(os.environ, {'_PYI_APPLICATION_HOME_DIR': td}, clear=True):
            self.assertTrue(recover_bundle(td, '0.3.5'))
            args, kwargs = start.call_args
            self.assertEqual(args[0][1:], ['--config', 'settings with spaces.json'])
            self.assertEqual(kwargs['env']['PYINSTALLER_RESET_ENVIRONMENT'], '1')
            self.assertEqual(kwargs['env']['MACROSS_BUNDLE_RECOVERY'], '0.3.5')

    def test_current_bundle_does_not_restart(self):
        with tempfile.TemporaryDirectory() as td, patch('sys.frozen', True, create=True), \
                patch('bundle_runtime.subprocess.Popen') as start:
            assets = Path(td)/'assets'; assets.mkdir()
            (assets/'bundle-version.txt').write_text('0.3.5\n')
            self.assertFalse(recover_bundle(td, '0.3.5'))
            start.assert_not_called()

    def test_incomplete_recovery_stops_without_restart_loop(self):
        with tempfile.TemporaryDirectory() as td, patch('sys.frozen', True, create=True), \
                patch.dict(os.environ, {'MACROSS_BUNDLE_RECOVERY': '0.3.5'}), \
                patch('bundle_runtime.subprocess.Popen') as start:
            with self.assertRaises(RuntimeError):
                recover_bundle(td, '0.3.5')
            start.assert_not_called()

    def test_missing_corrupt_and_valid_artwork(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td)/'hero.jpg'
            self.assertEqual(load_hero(path).size, (1030, 260))
            path.write_bytes(b'corrupt')
            self.assertEqual(load_hero(path).size, (1030, 260))
            Image.new('RGB', (4, 3), 'red').save(path)
            loaded = load_hero(path)
            path.unlink()
            self.assertEqual(loaded.size, (4, 3))
            self.assertGreater(loaded.getpixel((0, 0))[0], 240)


if __name__ == '__main__':
    unittest.main()
