"""Lossless edits of the game's flat settings file (not ConfigParser INI).

The runtime uses fgets(128), sscanf(" %63[^=]=%63s"), and last accepted
assignment wins. In particular, spaces BEFORE '=' belong to the key, sections
are not meaningful, and three legacy boolean options use atoi, not strict 0/1.
Keep the original bytes, including BOM/comments/unknown keys, and append only
changed assignments so the runtime sees the requested value last.
"""
import os
import re
import tempfile

SCALES = ('1', '1.25', '1.5', '2', '2.5', '3')
RESOLUTIONS = dict(zip(SCALES, ('1280 × 720', '1600 × 900', '1920 × 1080',
                              '2560 × 1440', '3200 × 1800', '3840 × 2160 (4K)')))
DEFAULTS = dict(render_scale='1.5', fullscreen='0', vsync='1', bloom='1', subtitles='1', hd_textures='0')


def parse(raw):
    values = dict(DEFAULTS)
    # MSVCRT text mode translates CRLF and treats Ctrl-Z as EOF.
    text = raw.replace(b'\r\n', b'\n').split(b'\x1a', 1)[0].decode('latin1')
    for line in re.findall(r'[^\n]*\n|[^\n]+$', text):
        for start in range(0, len(line), 127):
            chunk = line[start:start+127].split('\0', 1)[0]
            match = re.match(r'\s*([^=]{1,63})=\s*([^\s]{1,63})', chunk, re.ASCII)
            if not match:
                continue
            key, value = match.groups()
            if key == 'render_scale' and value in SCALES:
                values[key] = value
            elif key in ('fullscreen', 'vsync', 'bloom'):
                number = re.match(r'[+-]?\d+', value, re.ASCII)
                values[key] = '1' if number and int(number[0]) != 0 else '0'
            elif key in ('subtitles', 'hd_textures') and value in ('0', '1'):
                values[key] = value
    return values


def read(path):
    try:
        return path.read_bytes()
    except FileNotFoundError:
        return None


def save(path, original, changes):
    for key, value in changes.items():
        if key not in DEFAULTS or value not in (SCALES if key == 'render_scale' else ('0', '1')):
            raise ValueError('Invalid launch option: ' + key)
    if read(path) != original:
        raise ValueError('Settings changed; reopen Launch options before saving.')
    current = parse(original or b'')
    changes = {key: value for key, value in changes.items() if value != current[key]}
    if not changes:
        return
    raw = original or b''
    # Appended values cannot be effective past a text-mode EOF marker.
    if b'\x1a' in raw:
        raise ValueError('Settings contain a text EOF marker; file was preserved.')
    newline = b'\r\n' if b'\r\n' in raw else b'\n'
    result = raw + (newline if raw and not raw.endswith(b'\n') else b'')
    result += b''.join(key.encode('ascii') + b'=' + value.encode('ascii') + newline
                       for key, value in changes.items())
    if any(parse(result)[key] != value for key, value in changes.items()):
        raise ValueError('Settings could not be represented safely.')
    pending = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix='.settings-', suffix='.tmp', delete=False) as stream:
            pending = stream.name
            stream.write(result)
            stream.flush()
            os.fsync(stream.fileno())
        if read(path) != original:
            raise ValueError('Settings changed; reopen Launch options before saving.')
        os.replace(pending, path)
    finally:
        if pending and os.path.exists(pending):
            os.unlink(pending)
