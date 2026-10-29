"""Apply reviewed host hooks to locally generated PPU code, with exact guards."""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def digest(text):
    return hashlib.sha256(text.encode('utf8')).hexdigest()


def apply(root=ROOT):
    recipe = json.loads((root / 'tools/local_build/ppu_hooks.json').read_text(encoding='utf8'))
    pending = []
    for row in recipe['files']:
        path = root / 'native/macross30' / row.get('directory', 'recompiled') / row['file']
        original = path.read_text(encoding='utf8')
        if digest(original) == row['output_sha256']:
            continue
        if digest(original) != row['input_sha256']:
            raise ValueError('Generated PPU source changed: ' + row['file'])
        lines = original.splitlines(keepends=True)
        for edit in reversed(row['edits']):
            start, count = edit['start'], edit['remove']
            if start < 0 or start + count > len(lines) or digest(''.join(lines[start:start+count])) != edit['removed_sha256']:
                raise ValueError('Host hook source guard failed: ' + row['file'])
            lines[start:start+count] = [edit['insert']]
        result = ''.join(lines)
        if digest(result) != row['output_sha256']:
            raise ValueError('Host hook result mismatch: ' + row['file'])
        pending.append((path, result))
    for path, result in pending:
        path.write_text(result, encoding='utf8', newline='\n')
    print('Verified all PPU host hooks: timing, Lua unwind, captions, probes and wait backoff', flush=True)


if __name__ == '__main__':
    apply()
