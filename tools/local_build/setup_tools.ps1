param([Parameter(Mandatory=$true)][string]$Root,[switch]$SkipCompiler,[switch]$SkipOriginalTool)
$ErrorActionPreference='Stop'
[Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12
$Root=[IO.Path]::GetFullPath($Root)
New-Item -ItemType Directory -Force -Path $Root | Out-Null
$sourceRoot=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$cachedPython=Join-Path $Root 'python\python.exe'
if(Test-Path -LiteralPath $cachedPython){$env:PATH=(Split-Path $cachedPython -Parent)+';'+$env:PATH}
$python=Get-Command python -ErrorAction SilentlyContinue
$usablePython=$false
if($python -and $python.Source -notlike '*WindowsApps*') { & $python.Source -c 'import sys; assert sys.version_info >= (3,11)' 2>$null; $usablePython=($LASTEXITCODE -eq 0) }
if(-not $usablePython) {
 Write-Output 'LOCAL_SETUP: Python (official Python.org embedded runtime)'
 $archive=Join-Path $Root 'python.zip'
 Invoke-WebRequest 'https://www.python.org/ftp/python/3.14.10/python-3.14.10-embed-amd64.zip' -OutFile $archive -UseBasicParsing
 if((Get-FileHash -LiteralPath $archive).Hash.ToLowerInvariant() -ne '90905e64a840557d8c1bc4f5b74db3d7ee85e3ffb217611cac1c7813acea4159'){throw 'Python download SHA-256 mismatch'}
 $pythonHome=Join-Path $Root 'python'
 Expand-Archive -LiteralPath $archive -DestinationPath $pythonHome -Force
 $pathFile=Join-Path $pythonHome 'python314._pth'
 [IO.File]::WriteAllLines($pathFile,@('python314.zip','.',(Join-Path $sourceRoot 'vendor\ps3recomp-master\tools'),(Join-Path $sourceRoot 'tools\local_build'),(Join-Path $sourceRoot 'tools\play')),(New-Object Text.UTF8Encoding($false)))
 $env:PATH=$pythonHome+';'+$env:PATH
}
# Cached embedded Python must follow the currently verified installer workspace,
# not the path of a previous program ZIP.
if(Test-Path -LiteralPath $cachedPython) {
 $cachedPathFile=Join-Path (Split-Path $cachedPython -Parent) 'python314._pth'
 [IO.File]::WriteAllLines($cachedPathFile,@('python314.zip','.',(Join-Path $sourceRoot 'vendor\ps3recomp-master\tools'),(Join-Path $sourceRoot 'tools\local_build'),(Join-Path $sourceRoot 'tools\play')),(New-Object Text.UTF8Encoding($false)))
}
if(-not $SkipCompiler) {
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot=$null
if(Test-Path -LiteralPath $vswhere){$vsRoot=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath}
$compilerReady=$vsRoot -and (Test-Path -LiteralPath (Join-Path $vsRoot 'VC\Tools\Llvm\x64\bin\clang-cl.exe')) -and (Test-Path -LiteralPath (Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'))
if(-not $compilerReady) {
 Write-Output 'LOCAL_SETUP: Microsoft C++ Build Tools; Windows may request installation permission'
 $installer=Join-Path $Root 'vs_BuildTools.exe'
 Invoke-WebRequest 'https://aka.ms/vs/stable/vs_BuildTools.exe' -OutFile $installer -UseBasicParsing
 $signature=Get-AuthenticodeSignature -LiteralPath $installer
 if($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notlike '*Microsoft Corporation*'){throw 'Microsoft build-tool signature verification failed'}
 $child=Start-Process -FilePath $installer -ArgumentList '--passive','--wait','--norestart','--add','Microsoft.VisualStudio.Workload.VCTools','--add','Microsoft.VisualStudio.Component.VC.Llvm.Clang','--add','Microsoft.VisualStudio.Component.VC.CMake.Project','--includeRecommended' -WindowStyle Hidden -PassThru
 $child.WaitForExit()
 if($child.ExitCode -ne 0 -and $child.ExitCode -ne 3010){throw ('Build tools installation failed: '+$child.ExitCode)}
}
}
$rpcs3Home=Join-Path $Root 'rpcs3'
if(-not $SkipOriginalTool -and -not (Test-Path -LiteralPath (Join-Path $rpcs3Home 'rpcs3.exe'))) {
 Write-Output 'LOCAL_SETUP: official RPCS3 binary preparation utility (no game data or firmware)'
 $release=Invoke-RestMethod 'https://api.github.com/repos/RPCS3/rpcs3-binaries-win/releases/latest' -Headers @{'User-Agent'='Macross30-LocalBuild'}
 $asset=$release.assets | Where-Object {$_.name -match '_win64.*\.7z$'} | Select-Object -First 1
 if(-not $asset -or $asset.digest -notmatch '^sha256:[a-f0-9]{64}$' -or $asset.browser_download_url -notlike 'https://github.com/RPCS3/rpcs3-binaries-win/releases/download/*'){throw 'Official original-preparation utility metadata invalid'}
 $archive=Join-Path $Root 'rpcs3.7z'
 Invoke-WebRequest $asset.browser_download_url -OutFile $archive -UseBasicParsing -TimeoutSec 600
 if((Get-Item -LiteralPath $archive).Length -ne $asset.size -or 'sha256:'+(Get-FileHash -LiteralPath $archive).Hash.ToLowerInvariant() -ne $asset.digest){throw 'Official utility hash mismatch'}
 $seven=Join-Path $Root '7zr.exe'
 Invoke-WebRequest 'https://www.7-zip.org/a/7zr.exe' -OutFile $seven -UseBasicParsing
 if((Get-FileHash -LiteralPath $seven).Hash.ToLowerInvariant() -ne '256feca8e274e5da655e2a284fabafd9f554365eb164862089dacd4e8276d282'){throw '7-Zip utility hash mismatch'}
 & $seven x "-o$rpcs3Home" -y $archive
 if($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath (Join-Path $rpcs3Home 'rpcs3.exe'))){throw 'Official utility extraction failed'}
}
Write-Output 'LOCAL_SETUP_COMPLETE'
