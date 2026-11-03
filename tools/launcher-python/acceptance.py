"""Run the real packaged backend against an explicitly supplied acceptance config."""
import json
from pathlib import Path
import time
import traceback

from backend import Backend, VERSION


def run(config, report, data, staged_assets=None):
    config, report = Path(config).resolve(), Path(report).resolve()
    if not config.is_file():
        raise ValueError('Acceptance test requires an explicit config')
    report.parent.mkdir(parents=True, exist_ok=True)
    events = report.with_suffix('.events.jsonl')
    initial = json.loads(config.read_text(encoding='utf-8-sig'))
    if not Path(initial.get('iso', '')).is_file() or not Path(initial.get('folder', '')).is_absolute():
        raise ValueError('Acceptance ISO and absolute install path are required')
    def emit(event):
        with events.open('a', encoding='utf-8') as stream:
            stream.write(json.dumps({'time': time.time(), **event}, ensure_ascii=False)+'\n')
    opener = None
    if staged_assets:
        from urllib.parse import urlparse, unquote
        assets = Path(staged_assets).resolve()
        def opener(request, timeout=None):
            name = Path(unquote(urlparse(request.full_url).path)).name
            return (assets/name).open('rb')
    backend = Backend(data/'bridge.exe', config, emit=emit, confirm=lambda _: True, opener=opener)
    started = time.time()
    result = {'launcher': VERSION, 'config': str(config), 'install': initial['folder'],
              'language': backend.config['game_language'], 'started': started}
    try:
        backend.play()
        if not backend.installed.get('core_ready') or not backend.installed.get('language_ready'):
            raise RuntimeError('Installed files changed or failed verification after game exit')
        result.update(ok=True, installed=backend.installed, elapsed=time.time()-started)
    except Exception as error:
        result.update(ok=False, error=str(error), traceback=traceback.format_exc(), elapsed=time.time()-started)
    report.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    return 0 if result['ok'] else 1
