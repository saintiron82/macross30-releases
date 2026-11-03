"""Prepare Japanese original-data installer addon; never changes live game data."""
import hashlib
import json
from pathlib import Path
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / 'tools/dist'), str(ROOT / 'tools/play')]
from make_dist import make_delta, apply_delta_sha


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    dist = Path('C:/GameDev/Macross-dist-0.1.0/Macross30')
    output = Path('C:/GameDev/Macross-language-0.1.0')
    stage = output / 'languages/ja-JP/0.1.0'
    stage.mkdir(parents=True, exist_ok=True)
    originals = {r['path'].casefold(): r for r in json.loads((ROOT / 'native/vfs/source_manifest.json').read_text(encoding='utf8'))['files']}
    rows = []
    for line in (dist / 'korean/install-plan.tsv').read_text(encoding='utf8').splitlines():
        if line.startswith('#') or not line:
            continue
        kind, target, size, digest, source, delta = line.split('\t')
        if kind in ('iso', 'delta'):
            original = originals[source.casefold()]
            kind, size, digest, delta = 'iso', str(original['size']), original['source_sha256'], ''
        rows.append([kind, target, size, digest, source, delta])
    by_target = {r[1]: r for r in rows}
    for row in rows:
        if row[0] == 'copy':
            row[2:4] = by_target[row[4]][2:4]
    plan = '# Japanese original-data install plan\n' + ''.join('\t'.join(row) + '\n' for row in rows)
    (stage / 'install-plan.tsv').write_text(plan, encoding='utf8')
    source = dist / 'bin/EBOOT.ko.elf'
    target = ROOT / 'analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf'
    delta, _ = make_delta(source, target, sha(source), sha(target), 2048)
    assert apply_delta_sha(delta, source) == (target.stat().st_size, sha(target))
    (stage / 'EBOOT.mkdelta').write_bytes(delta)
    script = r'''param([Parameter(Mandatory=$true)][string]$Root,[Parameter(Mandatory=$true)][string]$Iso,[Parameter(Mandatory=$true)][string]$Launcher,[switch]$DryRun)
$ErrorActionPreference='Stop'
$env:PSModulePath=@((Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\Modules'),(Join-Path $env:ProgramFiles 'WindowsPowerShell\Modules')) -join ';'
$meta=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'language.json') -Raw | ConvertFrom-Json
foreach($name in @('install-plan.tsv','EBOOT.mkdelta','install.ps1')) {
 if((Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $name) -Algorithm SHA256).Hash.ToLowerInvariant() -ne $meta.files.$name) { throw 'Language addon hash mismatch' }
}
Add-Type -TypeDefinition ([IO.File]::ReadAllText((Join-Path $Root 'tools\DistInstaller.cs'))) -Language CSharp
$mode=if($DryRun){'dryrun'}else{'install'}
$problems=[MacrossDist.Installer]::Run($Root,$Root,(Join-Path $PSScriptRoot 'install-plan.tsv'),$Iso,$mode,$false,'',$meta.data_id)
if($problems -ne 0){exit 1}
if(-not $DryRun){
 $child=Start-Process -FilePath $Launcher -ArgumentList '--prepare-language',('"'+$Root+'"'),'ja-JP' -WindowStyle Hidden -PassThru
 $child.WaitForExit(); if($child.ExitCode -ne 0){throw 'Language runtime preparation failed'}
}
exit 0
'''
    (stage / 'install.ps1').write_bytes(script.replace('\n', '\r\n').encode('utf-8-sig'))
    metadata = dict(locale='ja-JP', game_version='0.1.0', data_id=hashlib.sha256(plan.encode()).hexdigest()[:16],
                    base_elf_sha256=sha(source), elf_sha256=sha(target),
                    files={name: sha(stage / name) for name in ['install-plan.tsv', 'EBOOT.mkdelta', 'install.ps1']})
    (stage / 'language.json').write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf8')
    archive = output / 'Macross30-language-ja-JP-0.1.0.zip'
    with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED) as z:
        for file in sorted(stage.iterdir()):
            z.write(file, 'Macross30/' + file.relative_to(output).as_posix())
    catalog = dict(game_version='0.1.0', languages={
        'ko-KR': dict(available=True),
        'ja-JP': dict(available=True, asset=dict(file=archive.name, bytes=archive.stat().st_size, sha256=sha(archive))),
        'en-US': dict(available=False, status='in-development; never substitute Korean resources'),
    })
    (output / 'LANGUAGES.json').write_text(json.dumps(catalog, indent=2) + '\n', encoding='utf8')
    print(json.dumps(dict(archive=str(archive), bytes=archive.stat().st_size, original_plan_items=len(rows), elf_reapply='PASS')))


if __name__ == '__main__':
    main()
