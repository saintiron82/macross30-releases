param([Parameter(Mandatory=$true)][string]$Root,[string]$Iso,[Parameter(Mandatory=$true)][string]$Payload,[Parameter(Mandatory=$true)][string]$Program,[ValidateSet('ja-JP')][string]$Language='ja-JP',[string]$ReleaseVersion)
$ErrorActionPreference='Stop'
$env:PSModulePath=@((Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\Modules'),(Join-Path $env:ProgramFiles 'WindowsPowerShell\Modules')) -join ';'
$Root=[IO.Path]::GetFullPath($Root)
$Program=[IO.Path]::GetFullPath($Program)
$Payload=[IO.Path]::GetFullPath($Payload)
$recipe=Get-Content -LiteralPath (Join-Path $Payload 'recipe.json') -Raw | ConvertFrom-Json
$supported='44d7112cbb5527314eb6192e59d861dacfcd4099f301a6e82ffc1681b0782b5b'
if($recipe.original_elf_sha256 -ne $supported){throw 'Unsupported original identity in payload'}
$original=Join-Path $Root '.local-build\original\EBOOT.elf'
$hasOriginal=(Test-Path -LiteralPath $original)
if($hasOriginal){$hasOriginal=((Get-FileHash -LiteralPath $original).Hash.ToLowerInvariant() -eq $supported)}
Write-Output 'BOOT_STAGE 1/7 Preparing original-data tools; no compiler required'
$programInfo=Get-Content -LiteralPath (Join-Path $Program 'program-manifest.json') -Raw | ConvertFrom-Json
if($programInfo.mode -ne 'user-original-eboot'){throw 'Unsupported program mode'}
foreach($row in $programInfo.files){
 $programFile=[IO.Path]::GetFullPath((Join-Path $Program ($row.path -replace '/', '\')))
 if(-not $programFile.StartsWith($Program.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Program manifest path escaped cache'}
 if((Get-Item -LiteralPath $programFile).Length -ne $row.bytes -or (Get-FileHash -LiteralPath $programFile).Hash.ToLowerInvariant() -ne $row.sha256){throw ('Program file hash mismatch: '+$row.path)}
}
# RPCS3's official portable archive does not contain the MSVC CRT. Use only
# the verified runtime libraries from this program package on a clean PC.
$env:PATH=(Join-Path $Program 'bin')+';'+$env:PATH
& (Join-Path $PSScriptRoot 'setup_tools.ps1') -Root (Join-Path $Root '.local-build\tools') -SkipCompiler -SkipOriginalTool:$hasOriginal
Write-Output 'BOOT_STAGE 2/7 Checking cached original EBOOT'
if(-not $hasOriginal){
 & python -X utf8 (Join-Path $PSScriptRoot 'original_elf.py') --iso $Iso --output (Split-Path $original -Parent) --rpcs3 (Join-Path $Root '.local-build\tools\rpcs3\rpcs3.exe')
 if($LASTEXITCODE -ne 0){throw 'User-local original EBOOT preparation failed'}
} else {Write-Output 'BOOT_ORIGINAL_REUSED: original hash verified; ISO EBOOT extraction skipped'}
Write-Output 'BOOT_STAGE 3/7 Preparing original Japanese ELF from cached original'
& python -X utf8 (Join-Path $PSScriptRoot 'prebuilt_finalize.py') --action elf --root $Root --payload $Payload --original-elf $original --locale $Language
if($LASTEXITCODE -ne 0){throw 'Local EBOOT patch application failed'}
Write-Output 'BOOT_STAGE 4/7 Verifying and installing downloaded runtime'
& python -X utf8 (Join-Path $PSScriptRoot 'prebuilt_finalize.py') --action program --root $Root --program $Program
if($LASTEXITCODE -ne 0){throw 'Prebuilt program verification failed'}
Write-Output 'BOOT_STAGE 5/7 Preparing original game data'
$isoArgument=if($Iso){$Iso}else{'.'}
& python -X utf8 (Join-Path $PSScriptRoot 'language_views.py') --root $Root --payload $Payload --iso $isoArgument --locale $Language
if($LASTEXITCODE -ne 0){throw 'Language data preparation failed'}
Write-Output 'BOOT_STAGE 6/7 Preparing original game launch'
$finalArgs=@('-X','utf8',(Join-Path $PSScriptRoot 'prebuilt_finalize.py'),'--action','finalize','--root',$Root,'--program',$Program,'--payload',$Payload,'--original-elf',$original,'--locale',$Language)
if($ReleaseVersion){$finalArgs+=@('--release-version',$ReleaseVersion)}
Write-Output 'BOOT_STAGE 7/7 Verifying installed program and user-derived ELF files'
& python @finalArgs
if($LASTEXITCODE -ne 0){throw 'Prebuilt installation final verification failed'}
