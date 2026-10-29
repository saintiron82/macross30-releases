"""Export reviewed build source; reject prebuilt game code and generated lifts."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile
import zipfile

ROOT = Path(__file__).resolve().parents[2]
EXCLUDED = ('native/macross30/geom_c5b0_capture_contract.h', 'native/macross30/recompiled/', 'native/macross30/spu_posttask_native/', 'native/macross30/raw_jobs/',
            'native/macross30/spu_graphics/', 'native/macross30/spu_gameplay/', 'native/macross30/spu_notify/spu_recomp',
            'native/macross30/spu/', 'vendor/ps3recomp-master/runtime/spu/tests/')
ALLOWED = ('vendor/ps3recomp-master/', 'native/macross30/', 'tools/build/', 'tools/local_build/', 'tools/play/inspect_iso.py')
FORBIDDEN_SUFFIXES = {'.exe', '.dll', '.lib', '.obj', '.o', '.a', '.elf', '.bin', '.self', '.prx', '.sprx', '.inc', '.png', '.dds', '.pam', '.wav', '.at3', '.zip', '.7z', '.tar', '.gz', '.pdb', '.pyc'}


def allowed(name):
    generic_includes = {'vendor/ps3recomp-master/runtime/spu/spu_interp_tables.inc', 'vendor/ps3recomp-master/runtime/spu/spu_intr.inc'}
    return (name.startswith(ALLOWED) and not name.startswith(EXCLUDED) and '/tests/' not in name and
            '/docs/' not in name and '/__pycache__/' not in name and (Path(name).suffix.lower() not in FORBIDDEN_SUFFIXES or name in generic_includes) and
            not Path(name).name.startswith('spu_recomp'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    snapshot = subprocess.check_output(['git', 'archive', 'HEAD', 'vendor/ps3recomp-master', 'native/macross30', 'tools/build', 'tools/play/inspect_iso.py'], cwd=ROOT)
    committed = {}
    with tarfile.open(fileobj=io.BytesIO(snapshot)) as tar:
        for entry in tar:
            if entry.isfile() and allowed(entry.name):
                committed[entry.name] = tar.extractfile(entry).read()
    files = list(committed)
    # Local build scripts are owned new source, not generated game output.
    files += [p.relative_to(ROOT).as_posix() for p in (ROOT / 'tools/local_build').rglob('*') if p.is_file()]
    rows = []
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.output, 'w', compression=zipfile.ZIP_DEFLATED) as z:
        for name in sorted(set(filter(allowed, files))):
            path = ROOT / name
            data = path.read_bytes() if name.startswith('tools/local_build/') else committed[name]
            if data[:4] in (b'\x7fELF', b'SCE\0', b'MZ\x90\x00'):
                raise ValueError('Binary payload rejected: ' + name)
            z.writestr('source/' + name, data)
            rows.append(dict(path=name, bytes=len(data), sha256=hashlib.sha256(data).hexdigest()))
        z.writestr('source/source-manifest.json', json.dumps(dict(mode='user-local-generate-and-compile', files=rows), indent=2))
    print(json.dumps(dict(files=len(rows), archive=str(args.output), bytes=args.output.stat().st_size)))


if __name__ == '__main__':
    main()
