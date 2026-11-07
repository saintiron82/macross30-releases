"""Production cellGame HLE against synthetic long paths; never starts the game."""
import os
import hashlib
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
CLANG = Path('C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/Llvm/x64/bin/clang.exe')


@unittest.skipUnless(os.name == 'nt' and CLANG.is_file(), 'Windows Clang integration')
class CellGameHostPathTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        output = ROOT/'analysis/startup039-20261108'
        output.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='cellgame-path-', dir=output)
        cls.addClassCleanup(cls.temp.cleanup)
        cls.directory = Path(cls.temp.name)
        cls.exe = cls.directory/'check.exe'
        result = subprocess.run([str(CLANG), '-std=c17', '-O1', '-D_CRT_SECURE_NO_WARNINGS',
            '-I'+str(ROOT/'vendor/ps3recomp-master/include'),
            str(ROOT/'vendor/ps3recomp-master/libs/system/tests/test_cellGame_host_paths.c'),
            '-o', str(cls.exe)], capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stdout+result.stderr)

    def check_case(self, mode, long=True, existing=True):
        root = self.directory/hashlib.sha256(self._testMethodName.encode()).hexdigest()[:8]
        if long:
            root = root/'native/installed-language-cache/en-US-fix'/('4'*64)/'state/hdd0/game'
        content = root/'game' if mode in ('fallback','vfs-fallback') else root
        if existing:
            (content/'BLJS10184_INSTALL/USRDIR').mkdir(parents=True)
        result = subprocess.run([str(self.exe), str(root), mode], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        if long:
            self.assertGreater(len(str(root)), 127)
        return content, result.stdout

    def test_long_existing_data_is_detected(self):
        self.check_case('exists')

    def test_short_existing_data_is_unchanged(self):
        self.check_case('exists', long=False)

    def test_long_missing_data_is_still_reported_missing(self):
        self.check_case('missing', existing=False)

    def test_long_create_uses_requested_root_and_preserves_guest_buffer_size(self):
        content, _ = self.check_case('create', existing=False)
        self.assertTrue((content/'BLJS10184_INSTALL/USRDIR').is_dir())

    def test_long_hdd_root_fallback(self):
        self.check_case('fallback')

    def test_long_vfs_root_fallback(self):
        self.check_case('vfs-fallback')

    def test_long_host_content_permit_returns_short_guest_paths(self):
        self.check_case('permit')


if __name__ == '__main__':
    unittest.main()
