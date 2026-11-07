<#
Macross 30 QA distribution: bug report collector.
Zips play\stdout.log, stderr.log, settings.ini, keymap.ini, padmap.ini, process.json, exit.json, the
last runs in play\history, play\spu-misses, the save folder, process and
system information into bug-reports\Macross30-logs-<time>.zip.
#>
[CmdletBinding()]
param([string]$Note, [int]$History = 3, [switch]$NoOpen, [string[]]$SaveDirectory = @(), [string]$Session)
$ErrorActionPreference = 'Stop'
# A PowerShell 7 parent leaks its module path and Windows PowerShell 5.1 then
# fails to load its own Microsoft.PowerShell.Utility (Get-FileHash etc.).
$env:PSModulePath = @((Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\Modules'),
                      (Join-Path $env:ProgramFiles 'WindowsPowerShell\Modules')) -join ';'
$dist = Split-Path -Parent $PSScriptRoot
$play = Join-Path $dist 'play'
$logRoot = $play
if ($Session) {
    if ($Session -notmatch '^[a-f0-9]{32}$') { throw 'Invalid session identity' }
    $logRoot = Join-Path $play ('sessions\' + $Session)
    if (-not (Test-Path -LiteralPath (Join-Path $logRoot 'session.json'))) { throw 'Session snapshot is missing' }
}
$stamp = (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-' + $PID
$reports = Join-Path $dist 'bug-reports'
$staging = Join-Path ([IO.Path]::GetTempPath()) ("macross30-logs-" + $stamp + "-" + $PID)
New-Item -ItemType Directory -Force -Path $staging, $reports | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $staging 'play') | Out-Null

function Copy-Shared([string]$Source, [string]$Destination) {
    # The game may still be writing its logs; open with shared read/write.
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Destination) | Out-Null
    $in = [IO.File]::Open($Source, 'Open', 'Read', 'ReadWrite, Delete')
    try {
        $out = [IO.File]::Create($Destination)
        try { $in.CopyTo($out) } finally { $out.Dispose() }
    } finally { $in.Dispose() }
}

function Copy-Tree([string]$Source, [string]$Destination) {
    if (-not (Test-Path -LiteralPath $Source)) { return }
    $base = (Resolve-Path -LiteralPath $Source).ProviderPath.TrimEnd('\')
    Get-ChildItem -LiteralPath $base -Recurse -File -Force | ForEach-Object {
        Copy-Shared $_.FullName (Join-Path $Destination $_.FullName.Substring($base.Length + 1))
    }
}

$copied = @()
foreach ($name in @('stdout.log', 'stderr.log', 'settings.ini', 'keymap.ini', 'padmap.ini', 'process.json', 'exit.json', 'session.json')) {
    $path = Join-Path $logRoot $name
    if (Test-Path -LiteralPath $path) { Copy-Shared $path (Join-Path $staging "play\$name"); $copied += $name }
}
Get-ChildItem -LiteralPath $logRoot -Filter 'diagnostics*.log' -File -ErrorAction SilentlyContinue | ForEach-Object {
    Copy-Shared $_.FullName (Join-Path $staging ('play\' + $_.Name)); $copied += $_.Name
}
Get-ChildItem -LiteralPath $play -File -Force -ErrorAction SilentlyContinue |
    Where-Object { $_.Length -eq 0 } | Select-Object -ExpandProperty Name |
    Set-Content -LiteralPath (Join-Path $staging 'play\flag-files.txt') -Encoding UTF8
$historyRoot = Join-Path $play 'history'
if (-not $Session -and (Test-Path -LiteralPath $historyRoot)) {
    Get-ChildItem -LiteralPath $historyRoot -Directory | Sort-Object Name -Descending | Select-Object -First $History |
        ForEach-Object { Copy-Tree $_.FullName (Join-Path $staging ("play\history\" + $_.Name)) }
}
if (-not $Session) { Copy-Tree (Join-Path $play 'spu-misses') (Join-Path $staging 'play\spu-misses') }
$home00 = Join-Path $play 'gamedata\dev_hdd0\home\00000001'
# cellSaveData.c uses cwd/gamedata (independent of language-specific HDD mounts).
# Never include every save by default: the user selects individual save slots.
$saveRoot = Join-Path $home00 'savedata'
foreach ($slot in $SaveDirectory) {
    if ([string]::IsNullOrWhiteSpace($slot) -or $slot -match '[\\/:]' -or $slot -in @('.', '..')) { throw 'Invalid save slot' }
    $source = Join-Path $saveRoot $slot
    if (-not (Test-Path -LiteralPath $source -PathType Container)) { throw 'Selected save slot is missing' }
    $links = @(Get-Item -LiteralPath $source; Get-ChildItem -LiteralPath $source -Force -Recurse) |
        Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }
    if ($links) { throw 'Linked save paths are not accepted' }
    Copy-Tree $source (Join-Path $staging ('savedata\' + $slot))
}

$info = [ordered]@{
    collected = (Get-Date).ToString('o')
    note = $Note
    distribution = $dist
    copied_play_files = $copied
    selected_save_slots = @($SaveDirectory)
    session = $Session
}
foreach ($pair in @(@('data_version_expected', 'korean\data-version.txt'), @('data_version_installed', 'data\installed-version.txt'))) {
    $path = Join-Path $dist $pair[1]
    $info[$pair[0]] = if (Test-Path -LiteralPath $path) { (Get-Content -LiteralPath $path -TotalCount 1).Trim() } else { $null }
}
$manifestPath = Join-Path $dist 'program-manifest.json'
if (Test-Path -LiteralPath $manifestPath) {
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $info.program_version = $manifest.version
    $changed = @()
    foreach ($file in $manifest.files) {
        $p = Join-Path $dist $file.path
        if (-not (Test-Path -LiteralPath $p)) { $changed += $file.path; continue }
        if ((Get-Item -LiteralPath $p).Length -ne $file.size) { $changed += $file.path; continue }
        $stream = [IO.File]::OpenRead($p)
        try { $sha = -join ([Security.Cryptography.SHA256]::Create().ComputeHash($stream) | ForEach-Object { $_.ToString('x2') }) }
        finally { $stream.Dispose() }
        if ($sha -ne $file.sha256) { $changed += $file.path }
    }
    $info.program_files_changed = $changed
}
try {
    $info.game_processes = @(Get-CimInstance Win32_Process -Filter "Name='Macross30Recompiled.exe'" | ForEach-Object {
        [ordered]@{ pid = $_.ProcessId; started = $_.CreationDate.ToString('o'); working_set_mb = [math]::Round($_.WorkingSetSize / 1MB);
                    command = $_.CommandLine }
    })
    $os = Get-CimInstance Win32_OperatingSystem
    $info.os = "$($os.Caption) $($os.Version) build $($os.BuildNumber)"
    $info.ram_gb = [math]::Round($os.TotalVisibleMemorySize / 1MB, 1)
    $info.cpu = @(Get-CimInstance Win32_Processor | ForEach-Object { $_.Name.Trim() })
    $info.gpu = @(Get-CimInstance Win32_VideoController | ForEach-Object {
        [ordered]@{ name = $_.Name; driver = $_.DriverVersion; mode = "$($_.CurrentHorizontalResolution)x$($_.CurrentVerticalResolution)@$($_.CurrentRefreshRate)" }
    })
    $drive = Get-PSDrive -Name ($dist.Substring(0, 1))
    $info.free_disk_gb = [math]::Round($drive.Free / 1GB, 1)
} catch { $info.system_info_error = $_.Exception.Message }
try {
    $info.recent_crashes = @(Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000, 1001; StartTime = (Get-Date).AddDays(-7) } -ErrorAction Stop |
        Where-Object { $_.Message -like '*Macross30Recompiled*' } | Select-Object -First 10 |
        ForEach-Object { [ordered]@{ time = $_.TimeCreated.ToString('o'); id = $_.Id; message = $_.Message } })
} catch { $info.recent_crashes = @() }
$info | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $staging 'report-info.json') -Encoding UTF8

$zip = Join-Path $reports ("Macross30-logs-" + $stamp + ".zip")
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($staging, $zip, [IO.Compression.CompressionLevel]::Optimal, $false)
Remove-Item -LiteralPath $staging -Recurse -Force
Write-Host "Bug report: $zip"
if (-not $NoOpen) { Start-Process explorer.exe -ArgumentList "/select,`"$zip`"" }
