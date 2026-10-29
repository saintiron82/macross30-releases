"""Fetch SDL2 (official libsdl-org/SDL GitHub release) into vendor/SDL2.

cellPad uses SDL2's GameController API for DualSense, DualShock 4, Switch Pro,
Xbox and other pads over USB and Bluetooth (zlib license). The build picks it
up from vendor/SDL2 when present; without it cellPad is XInput only.

The release is pinned by version and SHA-256, and only the headers, the x64
import library, SDL2.dll and the license/readme are kept.

    python tools/build/fetch_sdl2.py
"""
import hashlib
import io
import shutil
import sys
import urllib.request
import zipfile
from pathlib import Path

VERSION = '2.32.10'
URL = f'https://github.com/libsdl-org/SDL/releases/download/release-{VERSION}/SDL2-devel-{VERSION}-VC.zip'
SHA256 = 'af347939395a58b365846aaea27391e69f9ec9d4dd650d6ac40802159b418a6e'
KEEP = ('include/', 'lib/x64/SDL2.dll', 'lib/x64/SDL2.lib', 'LICENSE.txt', 'README-SDL.txt')

ROOT = Path(__file__).resolve().parents[2]
DEST = ROOT / 'vendor/SDL2'


def main():
    stamp = DEST / 'VERSION.txt'
    if stamp.exists() and stamp.read_text().strip() == VERSION:
        print(f'SDL2 {VERSION} already in {DEST}')
        return 0
    print(f'downloading {URL}')
    data = urllib.request.urlopen(URL, timeout=120).read()
    got = hashlib.sha256(data).hexdigest()
    if got != SHA256:
        print(f'SHA-256 mismatch: got {got}, want {SHA256}', file=sys.stderr)
        return 1
    if DEST.exists():
        shutil.rmtree(DEST)
    prefix = f'SDL2-{VERSION}/'
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        for info in z.infolist():
            name = info.filename
            if info.is_dir() or not name.startswith(prefix):
                continue
            rel = name[len(prefix):]
            if not any(rel == k or (k.endswith('/') and rel.startswith(k)) for k in KEEP):
                continue
            out = DEST / rel
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_bytes(z.read(info))
    stamp.write_text(VERSION + '\n')
    print(f'SDL2 {VERSION} -> {DEST}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
