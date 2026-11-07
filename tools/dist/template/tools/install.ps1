<#
Macross 30 QA distribution: game data installer.

Extracts the original disc files from YOUR OWN decrypted Macross 30 disc image
(BLJS10184), rebuilds the Korean resource files from korean\deltas, builds the
HDD install tree and verifies every file against korean\install-plan.tsv.
Nothing from the original disc ships with this distribution.

  Install.cmd [path\to\game.iso]           (drag the ISO onto Install.cmd)
  powershell -File tools\install.ps1 -Iso X.iso [-DevFlash <dir>] [-LinkHdd]
  powershell -File tools\install.ps1 -Iso X.iso -DryRun     (read + hash only, writes nothing)
  powershell -File tools\install.ps1 -Verify                 (re-hash installed data)
#>
[CmdletBinding()]
param(
    [string]$Iso,
    [string]$DevFlash,
    [string]$Target,
    [switch]$LinkHdd,
    [switch]$DryRun,
    [switch]$Verify,
    [string]$Only
)
$ErrorActionPreference = 'Stop'
# A PowerShell 7 parent leaks its module path and Windows PowerShell 5.1 then
# fails to load its own Microsoft.PowerShell.Utility (Get-FileHash etc.).
$env:PSModulePath = @((Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\Modules'),
                      (Join-Path $env:ProgramFiles 'WindowsPowerShell\Modules')) -join ';'
$dist = Split-Path -Parent $PSScriptRoot
if (-not $Target) { $Target = $dist }
$Target = [IO.Path]::GetFullPath($Target)
$plan = Join-Path $dist 'korean\install-plan.tsv'
$versionFile = Join-Path $dist 'korean\data-version.txt'
foreach ($p in @($plan, $versionFile, (Join-Path $PSScriptRoot 'DistInstaller.cs'))) {
    if (-not (Test-Path -LiteralPath $p)) { throw "Missing distribution file: $p (is the Korean pack unpacked here?)" }
}
$dataVersion = (Get-Content -LiteralPath $versionFile -TotalCount 1).Trim()

function Select-Iso {
    $candidates = @(Get-ChildItem -LiteralPath $dist -Filter *.iso -File -ErrorAction SilentlyContinue)
    if ($candidates.Count -eq 1) { return $candidates[0].FullName }
    try {
        Add-Type -AssemblyName System.Windows.Forms
        $dialog = New-Object System.Windows.Forms.OpenFileDialog
        $dialog.Title = 'Select your Macross 30 (BLJS10184) disc image'
        $dialog.Filter = 'PS3 disc image (*.iso)|*.iso|All files (*.*)|*.*'
        if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { return $dialog.FileName }
    } catch { }
    return (Read-Host 'Path to the Macross 30 ISO').Trim('"')
}

if (-not $Verify) {
    if (-not $Iso) { $Iso = Select-Iso }
    if (-not $Iso -or -not (Test-Path -LiteralPath $Iso -PathType Leaf)) { throw "Disc image not found: $Iso" }
    $Iso = (Resolve-Path -LiteralPath $Iso).ProviderPath
}

Add-Type -TypeDefinition ([IO.File]::ReadAllText((Join-Path $PSScriptRoot 'DistInstaller.cs'))) -Language CSharp

# The localized EBOOT in bin\ must be the one this Korean pack was built for.
$pack = Get-Content -LiteralPath (Join-Path $dist 'korean\korean-pack.json') -Raw | ConvertFrom-Json
$elf = Join-Path $dist 'bin\EBOOT.ko.elf'
if (Test-Path -LiteralPath $elf) {
    $elfSha = [MacrossDist.Installer]::HashFile($elf)
    if ($elfSha -ne $pack.elf_sha256) {
        Write-Warning "bin\EBOOT.ko.elf ($elfSha) is not the EBOOT this Korean pack was built for ($($pack.elf_sha256)). Use matching program and Korean packs."
        if (-not $DryRun -and -not $Verify) { exit 2 }
    }
}

$mode = if ($Verify) { 'verify' } elseif ($DryRun) { 'dryrun' } else { 'install' }
Write-Host "Macross 30 data installer ($mode), data version $dataVersion"
Write-Host "  distribution: $dist"
Write-Host "  install root: $Target"
if ($Iso) { Write-Host "  disc image:   $Iso" }
$watch = [Diagnostics.Stopwatch]::StartNew()
$problems = [MacrossDist.Installer]::Run($dist, $Target, $plan, $Iso, $mode, [bool]$LinkHdd, $Only, $dataVersion)

if ($mode -eq 'install' -and $problems -eq 0) {
    foreach ($dir in @('data\hdd1', 'play', 'firmware\dev_flash')) {
        New-Item -ItemType Directory -Force -Path (Join-Path $Target $dir) | Out-Null
    }
    if ($DevFlash) {
        # Optional: the game has not been observed to read /dev_flash (see docs);
        # copy the user's own firmware tree (e.g. an RPCS3 dev_flash folder) if given.
        if (-not (Test-Path -LiteralPath $DevFlash -PathType Container)) { throw "dev_flash folder not found: $DevFlash" }
        Copy-Item -Path (Join-Path $DevFlash '*') -Destination (Join-Path $Target 'firmware\dev_flash') -Recurse -Force
        Write-Host "Copied firmware files from $DevFlash"
    }
}
Write-Host ("Finished in {0:N0} s, problems: {1}" -f $watch.Elapsed.TotalSeconds, $problems)
if ($problems -ne 0) { exit 1 }
if ($mode -eq 'install' -and -not $Only) { Write-Host 'Game data installed. Start the game with Macross30.cmd.' }
exit 0
