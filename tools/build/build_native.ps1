param(
    [int]$Jobs = 6,
    [switch]$SkipConfigure
)
$ErrorActionPreference = 'Stop'
$env:PYTHONUTF8 = '1'
$macrossWorkspace = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Push-Location $macrossWorkspace
try {
    $macrossVsWhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
    $macrossVsRoot = & $macrossVsWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $macrossVsRoot) { throw 'Visual Studio C++ build tools not found' }
    $macrossDevCmd = Join-Path $macrossVsRoot 'Common7\Tools\VsDevCmd.bat'
    $macrossEnvironment = & cmd.exe /d /s /c "`"$macrossDevCmd`" -no_logo -arch=x64 -host_arch=x64 >nul && set"
    if ($LASTEXITCODE -ne 0) { throw 'Visual Studio environment initialization failed' }
    foreach ($macrossLine in $macrossEnvironment) {
        if ($macrossLine -match '^([^=]+)=(.*)$') {
            [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
        }
    }
    $macrossClang = Join-Path $macrossVsRoot 'VC\Tools\Llvm\x64\bin\clang-cl.exe'
    $macrossNinja = Join-Path $macrossVsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
    $macrossSdk = Join-Path $macrossWorkspace 'vendor\ps3recomp-master'
    $macrossElf = Join-Path $macrossWorkspace 'analysis\extracted\PS3_GAME\USRDIR\EBOOT.elf'
    # cellPad's SDL2 controller backend (DualSense/DS4/Xbox ...): pinned official release.
    if (-not (Test-Path (Join-Path $macrossWorkspace 'vendor\SDL2\VERSION.txt'))) {
        & python -I (Join-Path $PSScriptRoot 'fetch_sdl2.py')
        if ($LASTEXITCODE -ne 0) { throw 'SDL2 fetch failed' }
    }
    if (-not $SkipConfigure) {
    & cmake -S native/macross30 -B native/build -G Ninja `
        "-DCMAKE_MAKE_PROGRAM=$macrossNinja" "-DCMAKE_C_COMPILER=$macrossClang" `
        "-DCMAKE_CXX_COMPILER=$macrossClang" "-DPS3RECOMP_DIR=$macrossSdk" `
        "-DPORT_SPU_INPUTS=$macrossElf" '-DCMAKE_BUILD_TYPE=Release' `
        '-DCMAKE_CXX_FLAGS_RELEASE=/O1 /DNDEBUG' '-DCMAKE_C_FLAGS_RELEASE=/O1 /DNDEBUG'
    if ($LASTEXITCODE -ne 0) { throw 'Native configure failed' }
    }
    & python -X utf8 (Join-Path $PSScriptRoot 'patch_geom2_local_dispatch.py') --apply
    if ($LASTEXITCODE -ne 0) { throw 'Validated geom2 local dispatch installation failed' }
    & cmake --build native/build --parallel $Jobs 2>&1 |
        Tee-Object -FilePath native/build-last.log |
        ForEach-Object { if ("$_" -match '^\[\d+/\d+\]|FAILED:|error:|ninja:') { $_ } }
    if ($LASTEXITCODE -ne 0) { throw 'Native build failed' }
}
finally {
    Pop-Location
}
