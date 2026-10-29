param([Parameter(Mandatory=$true)][string]$OriginalElf,[string]$Payload,[int]$Jobs=0,[switch]$SkipGenerate)
$ErrorActionPreference='Stop'
$env:PYTHONUTF8='1'
$buildRoot=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if(-not (Test-Path -LiteralPath $vswhere)){throw 'LOCAL_BUILD_TOOLS_REQUIRED: Install Visual Studio C++ Build Tools with Clang, CMake, Ninja and Windows SDK.'}
$vsRoot=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(-not $vsRoot){throw 'LOCAL_BUILD_TOOLS_REQUIRED: Visual Studio C++ Build Tools missing.'}
$devcmd=Join-Path $vsRoot 'Common7\Tools\VsDevCmd.bat'
$toolEnvironment=& cmd.exe /d /s /c "`"$devcmd`" -no_logo -arch=x64 -host_arch=x64 >nul && set"
if($LASTEXITCODE -ne 0){throw 'Build tool environment failed'}
foreach($line in $toolEnvironment){if($line -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($Matches[1],$Matches[2],'Process')}}
$clang=Join-Path $vsRoot 'VC\Tools\Llvm\x64\bin\clang-cl.exe'
$ninja=Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
if(-not (Get-Command python -ErrorAction SilentlyContinue)){throw 'LOCAL_BUILD_PYTHON_REQUIRED: Python 3.11 or later is required.'}
if($Jobs -le 0){try{$memoryGiB=(Get-CimInstance Win32_OperatingSystem).TotalVisibleMemorySize/1MB;$Jobs=[Math]::Max(1,[Math]::Min(6,[int][Math]::Floor($memoryGiB/3)))}catch{$Jobs=2}}
Push-Location $buildRoot
try {
 $generateArgs=@('-X','utf8','tools/local_build/prepare.py','--original-elf',$OriginalElf)
 if($SkipGenerate){$generateArgs+='--prepare-only'}
 if($Payload){$generateArgs+=@('--payload',$Payload)}
 & python @generateArgs
 if($LASTEXITCODE -ne 0){throw 'Local source generation failed'}
 & python -X utf8 tools/build/fetch_sdl2.py
 if($LASTEXITCODE -ne 0){throw 'SDL2 preparation failed'}
 $elf=Join-Path $buildRoot 'analysis\extracted\PS3_GAME\USRDIR\EBOOT.elf'
 & cmake -S native/macross30 -B native/build -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_C_COMPILER=$clang" "-DCMAKE_CXX_COMPILER=$clang" "-DPS3RECOMP_DIR=$buildRoot\vendor\ps3recomp-master" "-DPORT_SPU_INPUTS=$elf" '-DCMAKE_BUILD_TYPE=Release' '-DCMAKE_CXX_FLAGS_RELEASE=/O1 /DNDEBUG' '-DCMAKE_C_FLAGS_RELEASE=/O1 /DNDEBUG' '-DMACROSS_BUILD_POSTTASK_NATIVE=ON'
 if($LASTEXITCODE -ne 0){throw 'Local CMake configuration failed'}
 & cmake --build native/build --parallel $Jobs
 if($LASTEXITCODE -ne 0){throw 'Local native compilation failed'}
 [IO.File]::WriteAllText((Join-Path $buildRoot 'local-generation.ready'),(Get-FileHash -LiteralPath $OriginalElf).Hash.ToLowerInvariant())
 Write-Output 'LOCAL_BUILD_COMPLETE: native/build/Macross30Recompiled.exe'
} finally {Pop-Location}
