$ErrorActionPreference = 'Stop'
$env:PYTHONUTF8 = '1'
Push-Location (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent)
try {
    & python -X utf8 vendor/ps3recomp-master/tools/ppu_loader.py `
        analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf -o native/analysis
    if ($LASTEXITCODE -ne 0) { throw 'PPU analysis failed' }
    & python -X utf8 vendor/ps3recomp-master/tools/ppu_lifter.py `
        analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf `
        --functions native/analysis/EBOOT.functions.json `
        --hle-stubs native/analysis/EBOOT.imports.json `
        --code-end 0x8ddf4c --chunks 96 --jobs 4 `
        --output native/macross30/recompiled
    if ($LASTEXITCODE -ne 0) { throw 'PPU lifting failed' }
}
finally { Pop-Location }
