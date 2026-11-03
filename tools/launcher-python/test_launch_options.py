import tempfile
from pathlib import Path
import unittest
from unittest.mock import patch

import launch_options as options
from backend import Backend
from i18n import TEXT


class LaunchOptionsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root/'play').mkdir()
        self.path = self.root/'play/settings.ini'

    def test_runtime_parser_last_accepted_values_and_atoi(self):
        raw = (b'; comment\r\nrender_scale=2\r\nrender_scale=invalid\r\n'
               b'render_scale =3\r\nfullscreen=-2tail\r\nvsync=true\r\n'
               b'bloom=1\r\nbloom=nonsense\r\nsubtitles=0\r\nsubtitles=2\r\n')
        self.assertEqual(options.parse(raw), dict(render_scale='2', fullscreen='1', vsync='0', bloom='0', subtitles='0', hd_textures='0'))
        self.assertEqual(options.parse(b'render_scale=1.25 ; comment\n')['render_scale'], '1.25')
        self.assertEqual(options.parse(b'vsync=1\nvsync=0\n')['vsync'], '0')
        self.assertEqual(options.parse(b'\xef\xbb\xbfrender_scale=2\n')['render_scale'], '1.5')
        # fgets(128) can expose another assignment after 127 characters.
        self.assertEqual(options.parse(b'x'*127+b'bloom=0\n')['bloom'], '0')
        self.assertEqual(options.parse(b'\x00bloom=0\n')['bloom'], '1')
        self.assertEqual(options.parse(b'ignored\vbloom=0\n')['bloom'], '1')

    def test_all_scales_and_strict_save_ranges(self):
        for scale in options.SCALES:
            self.assertEqual(options.parse(('render_scale='+scale+'\n').encode())['render_scale'], scale)
        self.path.write_bytes(b'bloom=1\n')
        for changes in ({'render_scale':'4'}, {'render_scale':'1.0'}, {'bloom':'true'},
                        {'fullscreen':'2'}, {'language':'en-US'}, {'fps_unlock':'1'}, {'hd_textures':'2'}):
            with self.assertRaises(ValueError):
                options.save(self.path, b'bloom=1\n', changes)
            self.assertEqual(self.path.read_bytes(), b'bloom=1\n')

    def test_preserves_realistic_settings_bytes_comments_duplicates_and_language(self):
        raw = (b'\xef\xbb\xbf; settings\r\nrender_scale=1.5\r\nfullscreen=0\r\nvsync=1\r\n'
               b'fps_unlock=0\r\nbloom=0\r\nbloom=1 ; original\r\nsubtitles=1\r\n'
               b'language=ko-KR\r\nhd_textures=1\r\n; unknown \xff\r\nfuture_option=abc')
        self.path.write_bytes(raw)
        options.save(self.path, raw, {'bloom':'0', 'render_scale':'3', 'subtitles':'0'})
        saved = self.path.read_bytes()
        self.assertTrue(saved.startswith(raw+b'\r\n'))
        self.assertEqual(options.parse(saved)['render_scale'], '3')
        self.assertEqual(options.parse(saved)['bloom'], '0')
        self.assertEqual(options.parse(saved)['subtitles'], '0')
        for key in (b'language=', b'fps_unlock=', b'hd_textures='):
            self.assertEqual(saved.count(key), 1)

    def test_stale_file_and_replace_or_write_failure_preserve_file(self):
        original = b'vsync=1\n'
        self.path.write_bytes(original)
        with self.assertRaises(ValueError):
            options.save(self.path, b'old', {'vsync':'0'})
        for operation in ('launch_options.os.replace', 'launch_options.os.fsync'):
            with patch(operation, side_effect=OSError('fixture failure')):
                with self.assertRaises(OSError):
                    options.save(self.path, original, {'vsync':'0'})
            self.assertEqual(self.path.read_bytes(), original)
            self.assertEqual(list(self.path.parent.glob('.settings-*.tmp')), [])

    def test_text_eof_rejected_and_noop_does_not_write(self):
        original = b'bloom=1\n\x1abloom=0\n'
        self.path.write_bytes(original)
        with self.assertRaises(ValueError):
            options.save(self.path, original, {'bloom':'0'})
        with patch('launch_options.os.replace') as replace:
            options.save(self.path, original, {'bloom':'1'})
        replace.assert_not_called()
        self.assertEqual(self.path.read_bytes(), original)

    def test_concurrent_change_during_write_is_preserved(self):
        self.path.write_bytes(b'vsync=1\n')
        def external_edit(_):
            self.path.write_bytes(b'; external edit\nvsync=1\n')
        with patch('launch_options.os.fsync', side_effect=external_edit):
            with self.assertRaises(ValueError):
                options.save(self.path, b'vsync=1\n', {'vsync':'0'})
        self.assertEqual(self.path.read_bytes(), b'; external edit\nvsync=1\n')
        self.assertEqual(list(self.path.parent.glob('.settings-*.tmp')), [])

    def test_missing_file_write_failure_does_not_create_settings(self):
        with patch('launch_options.os.replace', side_effect=OSError('fixture failure')):
            with self.assertRaises(OSError):
                options.save(self.path, None, {'vsync':'0'})
        self.assertFalse(self.path.exists())
        self.assertEqual(list(self.path.parent.glob('.settings-*.tmp')), [])

    def backend(self):
        backend = Backend(self.root/'bridge.exe', self.root/'launcher-fixture.json')
        backend.config['folder'] = str(self.root)
        backend.bridge = lambda operation: {'stopped': True}
        return backend

    def test_configured_folder_missing_file_creation_and_no_config_or_save_changes(self):
        backend = self.backend()
        config = dict(backend.config)
        save = self.root/'play/save.dat'
        save.write_bytes(b'save fixture')
        doc = backend.load_launch_options()
        self.assertIsNone(doc['original'])
        backend.save_launch_options(doc, {'render_scale':'2.5'})
        self.assertEqual(self.path.read_bytes(), b'render_scale=2.5\n')
        self.assertEqual(backend.config, config)
        self.assertFalse(backend.config_path.exists())
        self.assertEqual(save.read_bytes(), b'save fixture')

    def test_backend_refuses_running_external_game_busy_folder_change_and_no_play(self):
        backend = self.backend()
        self.path.write_bytes(b'vsync=1\n')
        doc = backend.load_launch_options()
        for attr in ('running', 'busy'):
            setattr(backend, attr, True)
            with self.assertRaises(RuntimeError):
                backend.save_launch_options(doc, {'vsync':'0'})
            setattr(backend, attr, False)
        with patch.object(backend, 'bridge', side_effect=RuntimeError('external game running')):
            with self.assertRaises(RuntimeError):
                backend.save_launch_options(doc, {'vsync':'0'})
        backend.config['folder'] = str(self.root/'elsewhere')
        with self.assertRaises(ValueError):
            backend.save_launch_options(doc, {'vsync':'0'})
        with self.assertRaises(FileNotFoundError):
            backend.load_launch_options()
        self.assertFalse((self.root/'elsewhere').exists())
        self.assertEqual(self.path.read_bytes(), b'vsync=1\n')

    def test_locales_have_matching_launch_option_keys(self):
        keys = {key for key in TEXT['en'] if key.startswith(('option', 'launchOptions'))}
        for locale in ('ko', 'ja'):
            self.assertTrue(keys <= TEXT[locale].keys())

    def test_hd_only_written_with_explicit_core_capability(self):
        backend = self.backend()
        doc = backend.load_launch_options()
        backend.save_launch_options(doc, {'hd_textures': '1'})
        self.assertFalse(self.path.exists())
        backend.manifest = {'features': {'hd_textures': {'available': True}}}
        doc = backend.load_launch_options()
        backend.save_launch_options(doc, {'hd_textures': '1'})
        self.assertEqual(options.parse(self.path.read_bytes())['hd_textures'], '1')


if __name__ == '__main__':
    unittest.main()
