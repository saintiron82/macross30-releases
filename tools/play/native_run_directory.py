"""Keep diagnostic process logs separate from the user's normal game session."""
from pathlib import Path
import shutil


def prepare_run_directory(root, requested=None):
    root=Path(root).resolve()
    if requested is None:
        run=root/'native/play'
        run.mkdir(parents=True,exist_ok=True)
        return run
    run=Path(requested)
    run=(run if run.is_absolute() else root/run).resolve()
    if not run.is_relative_to(root/'native/analysis') or run==root/'native/analysis':
        raise ValueError('Diagnostic directory must be beneath native/analysis')
    if any((run/name).exists() for name in ('stdout.log','stderr.log','process.json')):
        raise ValueError('Diagnostic directory already contains process logs; choose a fresh directory')
    run.mkdir(parents=True,exist_ok=True)
    settings=root/'native/play/settings.ini'
    if settings.exists() and not (run/'settings.ini').exists():
        shutil.copyfile(settings,run/'settings.ini')
    return run
