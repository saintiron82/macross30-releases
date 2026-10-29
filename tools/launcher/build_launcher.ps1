param([string]$Output)
$ErrorActionPreference = 'Stop'
$compiler = Join-Path $env:SystemRoot 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$launcherVersion = [regex]::Match([IO.File]::ReadAllText((Join-Path $PSScriptRoot 'SelfUpdate.cs')), 'public const string Version = "([0-9.]+)"').Groups[1].Value
if (-not $Output) { $Output = "C:\GameDev\Macross-launcher-$launcherVersion" }
$destination = [IO.Path]::GetFullPath($Output)
New-Item -ItemType Directory -Force -Path $destination | Out-Null
& $compiler /nologo /target:winexe /optimize+ /platform:x64 "/out:$destination\Macross30Launcher.exe" /r:System.Windows.Forms.dll /r:System.Drawing.dll /r:System.Web.Extensions.dll /r:System.IO.Compression.dll /r:System.IO.Compression.FileSystem.dll (Join-Path $PSScriptRoot 'Launcher.cs') (Join-Path $PSScriptRoot 'SelfUpdate.cs') (Join-Path $PSScriptRoot 'UiText.cs') (Join-Path $PSScriptRoot 'GameLanguage.cs') (Join-Path $PSScriptRoot 'LocalBuild.cs')
if ($LASTEXITCODE -ne 0) { throw 'Launcher compile failed' }
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'README.txt') -Destination $destination -Force
$launcherExe = Join-Path $destination 'Macross30Launcher.exe'
$launcherAsset = "Macross30Launcher-$launcherVersion.exe"
Copy-Item -LiteralPath $launcherExe -Destination (Join-Path $destination $launcherAsset) -Force
@{ version = $launcherVersion; file = $launcherAsset; bytes = (Get-Item -LiteralPath $launcherExe).Length; sha256 = (Get-FileHash -LiteralPath $launcherExe -Algorithm SHA256).Hash.ToLowerInvariant() } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $destination 'LAUNCHER.json') -Encoding ascii
Compress-Archive -LiteralPath $launcherExe, (Join-Path $destination 'README.txt') -DestinationPath (Join-Path $destination "Macross30-launcher-$launcherVersion.zip") -Force
Get-FileHash -LiteralPath (Join-Path $destination 'Macross30Launcher.exe') -Algorithm SHA256
