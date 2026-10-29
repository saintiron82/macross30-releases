param([Parameter(Mandatory=$true)][string]$Root,[Parameter(Mandatory=$true)][string]$Iso,[string]$OriginalElf,[Parameter(Mandatory=$true)][string]$Payload,[Parameter(Mandatory=$true)][string]$Runtime,[ValidateSet('ko-KR','ja-JP')][string]$Language='ko-KR',[switch]$SkipGenerate)
$ErrorActionPreference='Stop'
$env:PSModulePath=@((Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\Modules'),(Join-Path $env:ProgramFiles 'WindowsPowerShell\Modules')) -join ';'
$buildRoot=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$Root=[IO.Path]::GetFullPath($Root)
$recipe=Get-Content -LiteralPath (Join-Path $Payload 'recipe.json') -Raw | ConvertFrom-Json
& (Join-Path $PSScriptRoot 'setup_tools.ps1') -Root (Join-Path $Root '.local-build\tools')
if(-not $OriginalElf) {
 $OriginalElf=Join-Path $Root '.local-build\original\EBOOT.elf'
 if(-not (Test-Path -LiteralPath $OriginalElf)) {
  & python -X utf8 (Join-Path $PSScriptRoot 'original_elf.py') --iso $Iso --output (Split-Path $OriginalElf -Parent) --rpcs3 (Join-Path $Root '.local-build\tools\rpcs3\rpcs3.exe')
  if($LASTEXITCODE -ne 0){throw 'User-local original preparation failed'}
 }
}
if((Get-FileHash -LiteralPath $OriginalElf -Algorithm SHA256).Hash.ToLowerInvariant() -ne $recipe.original_elf_sha256){throw 'USER_ORIGINAL_ELF_REQUIRED: Original ELF hash mismatch.'}
# Compilation runs entirely in the isolated, verified source workspace.
$reuseMarker=Join-Path $buildRoot 'local-generation.ready'
$reuse=(Test-Path -LiteralPath $reuseMarker) -and (Test-Path -LiteralPath (Join-Path $buildRoot 'native\build\Macross30Recompiled.exe'))
if($reuse){$reuse=([IO.File]::ReadAllText($reuseMarker) -eq $recipe.original_elf_sha256)}
& (Join-Path $PSScriptRoot 'build.ps1') -OriginalElf $OriginalElf -Payload $Payload -SkipGenerate:($SkipGenerate -or $reuse)
if($LASTEXITCODE -ne 0){throw 'Local compilation failed'}
Add-Type -TypeDefinition ([IO.File]::ReadAllText((Join-Path $Payload 'DistInstaller.cs'))) -Language CSharp
$languageInfo=$recipe.languages.$Language
$plan=Join-Path $Payload ($languageInfo.plan -replace '/', '\')
$problems=[MacrossDist.Installer]::Run($Payload,$Root,$plan,$Iso,'install',$false,'',$languageInfo.data_id)
if($problems -ne 0){throw 'User ISO installation failed'}
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$crt=Get-ChildItem -LiteralPath (Join-Path $vsRoot 'VC\Redist\MSVC') -Directory | Where-Object {$_.Name -match '^\d+\.\d+\.\d+$'} | Sort-Object {[version]$_.Name} -Descending | ForEach-Object { Get-ChildItem -Path (Join-Path $_.FullName 'x64\Microsoft.VC14*.CRT') -Directory } | Select-Object -First 1
if(-not $crt){throw 'Visual C++ runtime not found in the user toolchain'}
New-Item -ItemType Directory -Force -Path (Join-Path $Root 'bin') | Out-Null
Get-ChildItem -LiteralPath $crt.FullName -Filter '*.dll' | Copy-Item -Destination (Join-Path $Root 'bin') -Force
& python -X utf8 (Join-Path $PSScriptRoot 'finalize.py') --root $Root --payload $Payload --runtime $Runtime --original-elf $OriginalElf --locale $Language
if($LASTEXITCODE -ne 0){throw 'User-local output installation failed'}
Write-Output 'LOCAL_INSTALL_COMPLETE'
