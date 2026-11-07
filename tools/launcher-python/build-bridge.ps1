$ErrorActionPreference='Stop'
$launcher=Join-Path $PSScriptRoot '../launcher'
$sources=@('Launcher.cs','UiText.cs','GameLanguage.cs','GameOptions.cs','LocalBuild.cs','InstallProgress.cs','PrebuiltInstall.cs') | ForEach-Object {Join-Path $launcher $_}
$version=(Get-Content (Join-Path $PSScriptRoot 'version.txt') -Raw).Trim()
$temporary=Join-Path ([IO.Path]::GetTempPath()) ('MacrossBridgeSelfUpdate-'+[guid]::NewGuid().ToString('N')+'.cs')
$self=[IO.File]::ReadAllText((Join-Path $launcher 'SelfUpdate.cs')) -replace 'public const string Version = "[0-9.]+"',('public const string Version = "'+$version+'"')
[IO.File]::WriteAllText($temporary,$self)
$sources+=$temporary
$sources+=Join-Path $PSScriptRoot 'Bridge.cs'
$compiler=Join-Path $env:SystemRoot 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
try{& $compiler /nologo /target:exe /optimize+ /platform:x64 /main:BridgeProgram "/out:$PSScriptRoot/bridge.exe" /r:System.Windows.Forms.dll /r:System.Drawing.dll /r:System.Web.Extensions.dll /r:System.IO.Compression.dll /r:System.IO.Compression.FileSystem.dll @sources
if($LASTEXITCODE -ne 0){throw 'Bridge build failed'}}finally{Remove-Item -LiteralPath $temporary -Force}
