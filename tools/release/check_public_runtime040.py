"""Reject 0.4.0 public runtime inputs containing the F8 measurement module."""
import argparse
import hashlib
import json
from pathlib import Path


def check(folder):
    folder = Path(folder)
    rsp = (folder / 'link.rsp').read_text('utf8')
    symbols = (folder / 'Macross30Recompiled.map').read_text('utf8', errors='replace')
    data = (folder / 'Macross30Recompiled.exe').read_bytes()
    forbidden = {
        'response': [s for s in ['measure_window.cpp.obj'] if s.lower() in rsp.lower()],
        'symbols': [s for s in ['macross_measure_trigger', 'macross_measure_key_allowed'] if s in symbols],
        'activation': [s.decode() for s in [b'[measure] ready', b'[measure] done window=', b'measure.go',
                                          b'MACROSS_MEASURE_SECONDS', b'MACROSS_MEASURE_BEEP'] if s in data]
    }
    if any(forbidden.values()):
        raise ValueError('Public runtime contains F8 measurement functionality: ' + str(forbidden))
    return dict(passed=True, exe_sha256=hashlib.sha256(data).hexdigest(),
                f8_measurement_excluded=True, forbidden_matches=forbidden, game_started=False)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('folder', type=Path)
    a = p.parse_args()
    print(json.dumps(check(a.folder), ensure_ascii=False, indent=2))
