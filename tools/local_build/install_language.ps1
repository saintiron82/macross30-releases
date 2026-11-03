param([Parameter(Mandatory=$true)][string]$Root,[Parameter(Mandatory=$true)][string]$Payload,[string]$Iso,[Parameter(Mandatory=$true)][ValidateSet('ko-KR','en-US')][string]$Language)
$ErrorActionPreference='Stop'
$env:PSModulePath=@((Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\Modules'),(Join-Path $env:ProgramFiles 'WindowsPowerShell\Modules')) -join ';'
$Root=[IO.Path]::GetFullPath($Root)
$Payload=[IO.Path]::GetFullPath($Payload)
$original=Join-Path $Root '.local-build\original\EBOOT.elf'
if(-not (Test-Path -LiteralPath $original)){throw 'Prepare the original game before installing a language pack'}
Write-Output 'BOOT_STAGE 1/7 Preparing translation tools; common game will not be updated'
& (Join-Path $PSScriptRoot 'setup_tools.ps1') -Root (Join-Path $Root '.local-build\tools') -SkipCompiler -SkipOriginalTool
$isoArgument=if($Iso){$Iso}else{'.'}
& python -X utf8 (Join-Path $PSScriptRoot 'prebuilt_finalize.py') --action language --root $Root --payload $Payload --original-elf $original --locale $Language --iso $isoArgument
if($LASTEXITCODE -ne 0){throw 'Language pack installation failed; common game was not updated'}
