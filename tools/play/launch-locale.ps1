param([Parameter(Mandatory=$true)][string]$Root,[Parameter(Mandatory=$true)][string]$BaseElf,[Parameter(Mandatory=$true)][string]$BaseVfs,[switch]$Inspect,[string]$Report)
$ErrorActionPreference='Stop'
$installation=[IO.Path]::GetFullPath($Root).TrimEnd('\')
function FileSha256([string]$path) {
 $algorithm=[Security.Cryptography.SHA256]::Create()
 $stream=[IO.File]::OpenRead($path)
 try{return ([BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-','').ToLowerInvariant()}
 finally{$stream.Dispose();$algorithm.Dispose()}
}
function LocalPath([string]$relative) {
 if([IO.Path]::IsPathRooted($relative) -or $relative -match '(^|[\\/])\.\.([\\/]|$)|[\x00-\x1f]'){throw 'Invalid locale resource path'}
 $path=[IO.Path]::GetFullPath([IO.Path]::Combine($installation,$relative))
 if(-not $path.StartsWith($installation+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Locale resource escaped installation'}
 $cursor=$path
 while($cursor -and $cursor.Length -ge $installation.Length){
  if(Test-Path -LiteralPath $cursor){if((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Locale resources cannot contain links'}}
  $cursor=[IO.Path]::GetDirectoryName($cursor)
 }
 if(-not(Test-Path -LiteralPath $path)){throw 'Locale resource is missing'}
 return $path
}
try {
 $exe=LocalPath 'bin/Macross30Recompiled.exe'
 $elf=[IO.Path]::GetFullPath($BaseElf);$vfs=[IO.Path]::GetFullPath($BaseVfs)
 foreach($path in @($elf,$vfs)){if(-not $path.StartsWith($installation+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Base resource escaped installation'}}
 $overrides=@{}
 $profilePath=Join-Path $installation '.local-build/active-locale.json'
 if(Test-Path -LiteralPath $profilePath){
  $profile=(Get-Content -LiteralPath (LocalPath '.local-build/active-locale.json') -Raw -Encoding UTF8)|ConvertFrom-Json
  $allowed=@('schema','locale','runtime_exe_sha256','elf','vfs','subtitles','battle_catalog','elf_overlay','native_ui','hdd0_game')
  foreach($property in $profile.PSObject.Properties){if($property.Name -notin $allowed){throw 'Unknown locale launch field'}}
  if($profile.schema -ne 1 -or $profile.locale -notmatch '^[A-Za-z]{2,8}(-[A-Za-z0-9]{1,8})*$'){throw 'Invalid locale launch profile'}
  if((FileSha256 $exe) -ne $profile.runtime_exe_sha256){throw 'Update common runtime before starting this language'}
  $elf=LocalPath $profile.elf;$vfs=LocalPath $profile.vfs
  $overrides['MACROSS_GAME_LANGUAGE']=$profile.locale
  $overrides['MACROSS_UI_LANGUAGE']=$profile.locale
  $mapping=@{subtitles='MACROSS_SUBTITLE_DIR';battle_catalog='MACROSS_BATTLE_CATALOG_FILE';elf_overlay='MACROSS_ELF_TEXT_PACK_FILE';native_ui='MACROSS_UI_CATALOG_FILE';hdd0_game='PS3_HDD0_GAME_ROOT'}
  foreach($key in $mapping.Keys){if($profile.$key){$overrides[$mapping[$key]]=LocalPath $profile.$key}}
  $overrides['MACROSS_BATTLE_SUBTITLES']='1'
 }
 $play=LocalPath 'play'
 $summary=@{exe=$exe;elf=$elf;vfs=$vfs;cwd=$play;overrides=$overrides;started=$false}
 if($Inspect){
  if($Report){[IO.File]::WriteAllText($Report,($summary|ConvertTo-Json -Depth 5),[Text.UTF8Encoding]::new($false))}
  else{$summary|ConvertTo-Json -Depth 5}
  exit 0
 }
 $start=[Diagnostics.ProcessStartInfo]::new($exe,('"'+$elf+'" "'+$vfs+'"'))
 $start.WorkingDirectory=$play;$start.UseShellExecute=$false;$start.CreateNoWindow=$true
 $start.RedirectStandardInput=$true;$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
 foreach($key in $overrides.Keys){$start.EnvironmentVariables[$key]=$overrides[$key]}
 $process=[Diagnostics.Process]::Start($start);$process.StandardInput.Close()
 $language=$env:MACROSS_GAME_LANGUAGE
 if($overrides.ContainsKey('MACROSS_GAME_LANGUAGE')){$language=$overrides['MACROSS_GAME_LANGUAGE']}
 $record=@{pid=$process.Id;command=@($exe,$elf,$vfs);started=$process.StartTime.ToUniversalTime().ToString('o');language=$language;text_pack=$overrides.ContainsKey('MACROSS_GAME_LANGUAGE')}
 [IO.File]::WriteAllText((Join-Path $play 'process.json'),($record|ConvertTo-Json -Depth 4),[Text.UTF8Encoding]::new($false))
 $stdout=[IO.File]::Open((Join-Path $play 'stdout.log'),[IO.FileMode]::Create,[IO.FileAccess]::Write,[IO.FileShare]::Read)
 $stderr=[IO.File]::Open((Join-Path $play 'stderr.log'),[IO.FileMode]::Create,[IO.FileAccess]::Write,[IO.FileShare]::Read)
 try{
  $copyOut=$process.StandardOutput.BaseStream.CopyToAsync($stdout)
  $copyErr=$process.StandardError.BaseStream.CopyToAsync($stderr)
  $process.WaitForExit();[Threading.Tasks.Task]::WaitAll([Threading.Tasks.Task[]]@($copyOut,$copyErr))
  $exitCode=$process.ExitCode
 }finally{$stdout.Dispose();$stderr.Dispose();$process.Dispose()}
 exit $exitCode
}catch{
 $errorPath=Join-Path $installation 'play/stderr.log'
 if(Test-Path -LiteralPath ([IO.Path]::GetDirectoryName($errorPath))){[IO.File]::WriteAllText($errorPath,$_.Exception.Message,[Text.UTF8Encoding]::new($false))}
 exit 1
}
