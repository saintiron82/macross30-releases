"""First-run / after-update graphics pipeline preparation before launch.

Runs bin/MacrossShaderPrep.exe against the game's own shader cache folder: the
RSX_SHADER_CACHE_DIR that the installed Macross30.cmd sets (per-user
%LOCALAPPDATA%\\Macross30\\shader-cache-v2), or the renderer default
play/scratch/rsx_shader_cache_v2 when the script sets none. The tool
builds a local D3D12 pipeline library for this PC's GPU and driver from the
shipped PSO inventory plus PSOs the game recorded on earlier runs. It is fast
when nothing changed (status check only) and bounded otherwise.

Preparation is an optimisation only: missing tool, unsupported driver, errors
or cancellation never block the game, which then compiles/creates on demand
exactly as before.
"""
import json
import os
import re
import subprocess
import time
from pathlib import Path

NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
TOOL = 'bin/MacrossShaderPrep.exe'
CACHE = 'play/scratch/rsx_shader_cache_v2'
CANCEL = 'prep.cancel'
MAX_SECONDS = 180
CACHE_SETTING = re.compile(r'^\s*set "RSX_SHADER_CACHE_DIR=([^"\r\n]+)"\s*$', re.I | re.M)
VARIABLE = re.compile(r'%([^%\s]+)%')


def cache_dir(root, environ=None):
    """The cache folder the game launched by root/Macross30.cmd uses (last setting wins)."""
    root = Path(root)
    environ = os.environ if environ is None else environ
    try:
        script = (root / 'Macross30.cmd').read_text(encoding='utf-8', errors='replace')
    except OSError:
        script = ''
    settings = CACHE_SETTING.findall(script)
    if settings:
        known = {'_M30_ROOT': str(root)}
        unresolved = []

        def expand(match):
            name = match.group(1)
            value = known.get(name.upper(), environ.get(name))
            if value is None:
                unresolved.append(name)
                return match.group(0)
            return value
        value = VARIABLE.sub(expand, settings[-1])
        if not unresolved and Path(value).is_absolute():
            return Path(value)
    return root / CACHE


def paths(root, environ=None):
    root = Path(root)
    return root / TOOL, cache_dir(root, environ)


def parse_events(lines):
    for line in lines:
        line = line.strip()
        if line.startswith('{'):
            try:
                yield json.loads(line)
            except ValueError:
                continue


class Preparation:
    def __init__(self, root, popen=subprocess.Popen):
        self.tool, self.cache = paths(root)
        self.popen = popen
        self.process = None

    def available(self):
        return self.tool.is_file()

    def _run(self, args, on_event):
        self.cache.mkdir(parents=True, exist_ok=True)
        command = [str(self.tool), '--cache-dir', str(self.cache), *args]
        self.process = self.popen(command, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                  stdin=subprocess.DEVNULL, text=True, encoding='utf-8', errors='replace',
                                  creationflags=NO_WINDOW)
        last = None
        try:
            for event in parse_events(self.process.stdout):
                last = event
                on_event(event)
        finally:
            code = self.process.wait()
            if self.process.stdout:
                self.process.stdout.close()
            self.process = None
        return code, last

    def needed(self):
        """True when the local library is absent, stale, for another driver, or incomplete."""
        if not self.available():
            return False
        code, _ = self._run(['--status'], lambda event: None)
        return code == 10

    def cancel(self):
        """Ask a running preparation to stop at the next pipeline; it keeps what it finished."""
        try:
            (self.cache / CANCEL).write_text('cancel\n', encoding='ascii')
        except OSError:
            pass

    def prepare(self, on_progress=lambda done, total: None, max_seconds=MAX_SECONDS):
        """Returns a summary dict; never raises for tool failures."""
        if not self.available():
            return dict(status='absent')
        (self.cache / CANCEL).unlink(missing_ok=True)
        started = time.monotonic()
        try:
            if not self.needed():
                return dict(status='current', seconds=round(time.monotonic() - started, 2))

            def event(e):
                if e.get('event') in ('progress', 'checkpoint', 'done') and e.get('total'):
                    on_progress(e.get('processed', 0), e['total'])

            code, last = self._run(['--cancel-file', str(self.cache / CANCEL), '--max-seconds', str(max_seconds)], event)
        except OSError as error:
            return dict(status='error', error=str(error))
        finally:
            (self.cache / CANCEL).unlink(missing_ok=True)
        status = {0: 'complete', 2: 'cancelled', 3: 'unsupported'}.get(code, 'error')
        summary = dict(status=status, exit_code=code, seconds=round(time.monotonic() - started, 2))
        if last:
            summary.update({k: last.get(k) for k in ('total', 'prepared', 'created', 'already', 'missing_shader', 'failed')})
        return summary
