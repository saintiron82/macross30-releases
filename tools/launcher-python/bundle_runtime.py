"""Recover a new executable started with an older one-file extraction directory."""
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import time

from PIL import Image


def recover_bundle(data, version):
    if not getattr(sys, 'frozen', False):
        return False
    try:
        extracted_version = (Path(data)/'assets/bundle-version.txt').read_text().strip()
    except OSError:
        extracted_version = ''
    if extracted_version == version:
        return False
    # Older updater helpers do not reset PyInstaller's inherited private state.
    # A versioned marker distinguishes their stale extraction from our own.
    if os.environ.get('MACROSS_BUNDLE_RECOVERY') == version:
        raise RuntimeError('Launcher extraction is incomplete. Please extract the launcher ZIP again.')
    env = dict(os.environ, PYINSTALLER_RESET_ENVIRONMENT='1', MACROSS_BUNDLE_RECOVERY=version)
    child = subprocess.Popen([sys.executable, *sys.argv[1:]], env=env, cwd=os.getcwd())
    # Legacy helpers regard an early exit as an update failure. Keep this
    # handoff process alive until the clean instance acknowledges startup.
    if '--update-health' in sys.argv:
        index = sys.argv.index('--update-health')
        health = Path(sys.argv[index + 1])
        with open(sys.executable, 'rb') as stream:
            expected = hashlib.file_digest(stream, 'sha256').hexdigest()
        deadline = time.monotonic() + 14
        while time.monotonic() < deadline:
            try:
                ready = health.read_text(encoding='utf-8') == expected
            except OSError:
                ready = False
            if ready or child.poll() is not None:
                break
            time.sleep(.05)
    return True


def load_hero(path):
    try:
        with Image.open(path) as source:
            return source.convert('RGB')
    except OSError:
        # Artwork is optional; unavailable or corrupt artwork must not block play.
        return Image.new('RGB', (1030, 260), '#10141c')
