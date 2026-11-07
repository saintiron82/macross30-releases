<#
Macross 30 QA launcher helper (called by Macross30.cmd; failures never block the game).
  -Phase pre : move the previous stdout/stderr/process/exit files to play\history\<UTC time>
               and write play\process.json (same fields as tools/play/play_native.py, pid unknown).
  -Phase post: write play\exit.json with the exit code.
#>
param(
    [Parameter(Mandatory = $true)][ValidateSet('pre', 'post')][string]$Phase,
    [Parameter(Mandatory = $true)][string]$Play,
    [string]$Exe, [string]$Elf, [string]$Vfs, [string]$Uncapped, [int]$ExitCode
)
$ErrorActionPreference = 'Stop'
# A PowerShell 7 parent leaks its module path; use Windows PowerShell's own modules.
$env:PSModulePath = @((Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\Modules'),
                      (Join-Path $env:ProgramFiles 'WindowsPowerShell\Modules')) -join ';'
$utf8 = New-Object System.Text.UTF8Encoding($false)
$now = [DateTime]::UtcNow
$iso = $now.ToString("yyyy-MM-dd'T'HH:mm:ss.ffffff'+00:00'", [Globalization.CultureInfo]::InvariantCulture)
try {
    if ($Phase -eq 'pre') {
        if (Test-Path -LiteralPath (Join-Path $Play 'stdout.log')) {
            $history = Join-Path $Play ('history\' + $now.ToString("yyyyMMdd'T'HHmmssffffff'Z'", [Globalization.CultureInfo]::InvariantCulture))
            New-Item -ItemType Directory -Force -Path $history | Out-Null
            foreach ($name in @('stdout.log', 'stderr.log', 'process.json', 'exit.json')) {
                $path = Join-Path $Play $name
                if (Test-Path -LiteralPath $path) { Copy-Item -LiteralPath $path -Destination (Join-Path $history $name) }
            }
        }
        $exit = Join-Path $Play 'exit.json'
        if (Test-Path -LiteralPath $exit) { Remove-Item -LiteralPath $exit -Force }
        $process = [ordered]@{
            pid = $null
            command = @($Exe, $Elf, $Vfs)
            video_mode = $null
            uncapped = ($Uncapped -eq '1')
            automatic_timeout = $false
            movie_stop_probe = $false
            spu_miss_directory = (Join-Path $Play 'spu-misses')
            started = $iso
            launcher = 'Macross30.cmd'
        }
        [IO.File]::WriteAllText((Join-Path $Play 'process.json'), ($process | ConvertTo-Json -Depth 3), $utf8)
    } else {
        $result = [ordered]@{ exit_code = $ExitCode; ended = $iso }
        [IO.File]::WriteAllText((Join-Path $Play 'exit.json'), ($result | ConvertTo-Json), $utf8)
    }
} catch {
    Write-Host "launch-helper ($Phase): $($_.Exception.Message)"
}
exit 0
