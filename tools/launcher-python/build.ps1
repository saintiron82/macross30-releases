param([string]$Output='C:/GameDev/Macross-launcher-0.3.2',[string]$HeroPath=(Join-Path $PSScriptRoot 'artwork/interstellar-v1.png'),[string]$ManifestPath=(Join-Path $PSScriptRoot 'assets/preview-manifest.json'),[string]$StatusPath=(Join-Path $PSScriptRoot '../dist/content/STATUS.json'))
$ErrorActionPreference='Stop'
$repo=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$python=Join-Path $repo '.tools/launcher-python-venv/Scripts/python.exe'
if(-not(Test-Path -LiteralPath $python)){throw 'Prepare .tools/launcher-python-venv using requirements-build.txt'}
$assets=Join-Path $PSScriptRoot 'assets'
New-Item -ItemType Directory -Force -Path $assets|Out-Null
& $python (Join-Path $PSScriptRoot 'prepare_assets.py') $HeroPath (Join-Path $assets 'hero.jpg')
if($LASTEXITCODE -ne 0){throw 'Launcher artwork preparation failed'}
Copy-Item -LiteralPath $StatusPath -Destination (Join-Path $assets 'preview-status.json') -Force
if([IO.Path]::GetFullPath($ManifestPath) -ne [IO.Path]::GetFullPath((Join-Path $assets 'preview-manifest.json'))){
 Copy-Item -LiteralPath $ManifestPath -Destination (Join-Path $assets 'preview-manifest.json') -Force
}
$version=(Get-Content (Join-Path $PSScriptRoot 'version.txt') -Raw).Trim()
[IO.File]::WriteAllText((Join-Path $assets 'bundle-version.txt'),$version,[Text.UTF8Encoding]::new($false))
& $python (Join-Path $repo 'tools/release/build_iso_tool.py')
if($LASTEXITCODE -ne 0){throw 'ISO tool build failed'}
$isoTools=Join-Path $repo '.tools/iso-tools'
& (Join-Path $PSScriptRoot 'build-bridge.ps1')
& $python -m PyInstaller --noconfirm --clean --onefile --windowed --name Macross30Launcher --distpath $Output --workpath (Join-Path $PSScriptRoot 'build') --specpath $PSScriptRoot --paths (Join-Path $repo 'tools/release') --paths (Join-Path $repo 'tools/play') --paths (Join-Path $repo 'tools/local_build') --exclude-module original_elf --add-data ($isoTools+';iso-tools') --additional-hooks-dir (Join-Path $PSScriptRoot 'hooks') --add-data ((Join-Path $assets 'bundle-version.txt')+';assets') --add-data ((Join-Path $assets 'hero.jpg')+';assets') --add-data ((Join-Path $assets 'preview-manifest.json')+';assets') --add-data ((Join-Path $assets 'preview-status.json')+';assets') --add-binary ((Join-Path $PSScriptRoot 'bridge.exe')+';.') --exclude-module numpy --exclude-module matplotlib --exclude-module PySide6 --exclude-module PyQt6 --exclude-module PIL.ImageCms --exclude-module PIL._imagingcms --exclude-module PIL._avif --exclude-module PIL._webp (Join-Path $PSScriptRoot 'app.py')
if($LASTEXITCODE -ne 0){throw 'Native Python launcher build failed'}
$version=(Get-Content (Join-Path $PSScriptRoot 'version.txt') -Raw).Trim()
$exe=Join-Path $Output 'Macross30Launcher.exe'
$asset=Join-Path $Output "Macross30Launcher-$version.exe"
Copy-Item -LiteralPath $exe -Destination $asset -Force
$metadata=@{version=$version;file=[IO.Path]::GetFileName($asset);bytes=(Get-Item -LiteralPath $asset).Length;sha256=(Get-FileHash -LiteralPath $asset).Hash.ToLowerInvariant()}
[IO.File]::WriteAllText((Join-Path $Output 'PYTHON-LAUNCHER.json'),($metadata|ConvertTo-Json),[Text.UTF8Encoding]::new($false))
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'USAGE.txt') -Destination $Output -Force
Compress-Archive -LiteralPath $exe,(Join-Path $Output 'USAGE.txt') -DestinationPath (Join-Path $Output "Macross30-launcher-$version.zip") -Force
$metadata|ConvertTo-Json
