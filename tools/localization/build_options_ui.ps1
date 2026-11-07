param([string]$Root=(Resolve-Path "$PSScriptRoot/../..").Path)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
$bundle=Join-Path $Root 'localization/image_assets/menu-ui-v7'
$out=Join-Path $bundle 'replacements'
New-Item -ItemType Directory -Force $out | Out-Null
$metadata=Get-Content (Join-Path $bundle 'audit/controller27-final-rois.json') -Raw | ConvertFrom-Json
$sourcePath=Join-Path $Root 'analysis/assets/data/opt_text01.dds'
$glyphPath=Join-Path $bundle 'generated/controller-27.png'
function SHA([byte[]]$bytes) { [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant() }
[byte[]]$original=[IO.File]::ReadAllBytes($sourcePath)
if((SHA $original) -ne $metadata.source_raw_sha256 -or $original.Length -ne 262272) { throw 'Original DDS source changed' }
[byte[]]$result=$original.Clone()
$allowed=[bool[]]::new(256*256)
$sheet=[Drawing.Bitmap]::new([string]$glyphPath)
$bands=@(@(31,74),@(89,130),@(145,188),@(201,244),@(258,301),@(316,358),@(370,413),@(427,468),@(481,524),@(539,580),@(594,637),@(652,694),@(708,750),@(765,808),@(821,864),@(878,921),@(935,978),@(992,1035),@(1049,1092),@(1106,1149),@(1162,1206),@(1220,1262),@(1277,1319),@(1334,1376),@(1390,1432),@(1441,1482),@(1487,1529))
if($sheet.Width -ne 1024 -or $sheet.Height -ne 1536) { throw 'Glyph sheet dimensions changed' }
$lock=$sheet.LockBits([Drawing.Rectangle]::new(0,0,$sheet.Width,$sheet.Height),[Drawing.Imaging.ImageLockMode]::ReadOnly,[Drawing.Imaging.PixelFormat]::Format32bppArgb)
[byte[]]$source=[byte[]]::new($lock.Stride*$sheet.Height)
[Runtime.InteropServices.Marshal]::Copy($lock.Scan0,$source,0,$source.Length)
$sheet.UnlockBits($lock)
$proofs=@()
for($labelIndex=0;$labelIndex -lt $metadata.labels.Count;$labelIndex++) {
 $label=$metadata.labels[$labelIndex];$band=$bands[$labelIndex]
 $left=$sheet.Width;$right=-1;$top=$sheet.Height;$bottom=-1
 for($y=$band[0];$y -lt $band[1];$y++) { for($x=0;$x -lt $sheet.Width;$x++) {
  $i=4*($y*$sheet.Width+$x);$white=[Math]::Min($source[$i],[Math]::Min($source[$i+1],$source[$i+2]));$core=$source[$i+3]*$white/255.0
  if($core -ge 64) {$left=[Math]::Min($left,$x);$right=[Math]::Max($right,$x);$top=[Math]::Min($top,$y);$bottom=[Math]::Max($bottom,$y)}
 }}
 if($right -lt $left) { throw "Empty glyph row $labelIndex" }
 $cw=$right-$left+1;$ch=$bottom-$top+1
 $clean=[Drawing.Bitmap]::new($cw,$ch,[Drawing.Imaging.PixelFormat]::Format32bppArgb)
 for($y=0;$y -lt $ch;$y++) { for($x=0;$x -lt $cw;$x++) {
  $i=4*(($top+$y)*$sheet.Width+$left+$x);$white=[Math]::Min($source[$i],[Math]::Min($source[$i+1],$source[$i+2]));$core=$source[$i+3]*$white/255.0
  $a=if($core -lt 64){0}else{[int][Math]::Round(($core-64)*255/191)}
  $clean.SetPixel($x,$y,[Drawing.Color]::FromArgb($a,255,255,255))
 }}
 $roi=$label.candidate_safe_ROI;$bbox=$label.source_visible_bbox_alpha_ge64
 $x0=[int]$roi[0];$y0=[int]$roi[1];$x1=[int]$roi[2];$y1=[int]$roi[3]
 $dx=[Math]::Max($x0+1,[int]$bbox[0]);$height=[Math]::Min(([int]$bbox[3]-[int]$bbox[1]),($y1-$y0-1))
 $dy=[Math]::Max($y0,([int]$bbox[3]-$height));$width=[Math]::Min(($x1-$dx-1),[int][Math]::Round($cw*$height/[double]$ch))
 if($height -lt 1 -or $width -lt 1 -or $dy+$height -gt $y1) {throw 'Invalid destination'}
 $small=[Drawing.Bitmap]::new($width,$height,[Drawing.Imaging.PixelFormat]::Format32bppArgb)
 $g=[Drawing.Graphics]::FromImage($small);$g.CompositingMode=[Drawing.Drawing2D.CompositingMode]::SourceCopy
 $g.InterpolationMode=[Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic;$g.PixelOffsetMode=[Drawing.Drawing2D.PixelOffsetMode]::HighQuality
 $g.DrawImage($clean,[Drawing.Rectangle]::new(0,0,$width,$height));$g.Dispose();$clean.Dispose()
 # Use the most frequent brightest original text core color, retaining SP row colors.
 $hist=@{}
 for($y=$y0;$y -lt $y1;$y++) {for($x=$x0;$x -lt $x1;$x++) {
  $i=128+4*($y*256+$x)
  if($original[$i+3] -ge 180) { $key="$($original[$i]),$($original[$i+1]),$($original[$i+2])";$hist[$key]++ }
 }}
 $bright=0;foreach($key in $hist.Keys) {$parts=$key.Split(',');$bright=[Math]::Max($bright,[Math]::Max([int]$parts[0],[Math]::Max([int]$parts[1],[int]$parts[2])))}
 $color=($hist.GetEnumerator()|Where-Object {$p=$_.Name.Split(',');[Math]::Max([int]$p[0],[Math]::Max([int]$p[1],[int]$p[2])) -ge $bright-8}|Sort-Object Value -Descending|Select-Object -First 1).Name
 if(!$color) {throw 'No original core color'};$bgr=@($color.Split(',')|ForEach-Object {[byte]$_})
 for($y=$y0;$y -lt $y1;$y++) {for($x=$x0;$x -lt $x1;$x++) {
  $pi=$y*256+$x;if($allowed[$pi]) {throw 'Overlapping edit ROI'};$allowed[$pi]=$true
  $result[128+4*$pi+3]=0
 }}
 $visible=0
 for($y=0;$y -lt $height;$y++) {for($x=0;$x -lt $width;$x++) {
  $a=$small.GetPixel($x,$y).A;if(!$a){continue};$i=128+4*(($dy+$y)*256+$dx+$x)
  $result[$i]=$bgr[0];$result[$i+1]=$bgr[1];$result[$i+2]=$bgr[2];$result[$i+3]=$a;$visible++
 }}
 $small.Dispose();if(!$visible){throw 'Invisible glyph'}
 $proofs+=@{id=$label.id;Japanese=$label.source;Korean=$label.target;edit_ROI_ltrb=$roi;source_crop_xywh=@($left,$top,$cw,$ch);destination_xywh=@($dx,$dy,$width,$height);original_BGR=$bgr;visible_pixels=$visible;ARK_TXOS=$label.ARK_TXOS;reference_status=$label.ROI_status}
}
$headerChanged=0;$outsideChanged=0;$changed=0
for($i=0;$i -lt $original.Length;$i++){if($original[$i] -ne $result[$i]){$changed++;if($i -lt 128){$headerChanged++}elseif(!$allowed[[int][Math]::Floor(($i-128)/4)]){$outsideChanged++}}}
if($headerChanged -or $outsideChanged){throw 'Header/nonROI changed'}
[IO.File]::WriteAllBytes((Join-Path $out 'opt_text01.dds'),$result)
$preview=[Drawing.Bitmap]::new(256,256,[Drawing.Imaging.PixelFormat]::Format32bppArgb)
$l=$preview.LockBits([Drawing.Rectangle]::new(0,0,256,256),[Drawing.Imaging.ImageLockMode]::WriteOnly,[Drawing.Imaging.PixelFormat]::Format32bppArgb)
[Runtime.InteropServices.Marshal]::Copy($result,128,$l.Scan0,262144);$preview.UnlockBits($l)
$preview.Save((Join-Path $out 'opt_text01.png'),[Drawing.Imaging.ImageFormat]::Png)
$black=[Drawing.Bitmap]::new(1024,1024);$g=[Drawing.Graphics]::FromImage($black);$g.Clear([Drawing.Color]::Black)
$g.InterpolationMode=[Drawing.Drawing2D.InterpolationMode]::NearestNeighbor;$g.PixelOffsetMode=[Drawing.Drawing2D.PixelOffsetMode]::Half
$g.DrawImage($preview,0,0,1024,1024);$black.Save((Join-Path $out 'opt_text01-black4x.png'));$g.Dispose();$black.Dispose();$preview.Dispose();$sheet.Dispose()
@{status='OFFLINE_DDS_CANDIDATE_NOT_INSTALLED';source=$sourcePath;source_sha256=(SHA $original);output_sha256=(SHA $result);glyph_sheet=$glyphPath;glyph_sheet_sha256=(SHA ([IO.File]::ReadAllBytes($glyphPath)));source_header_changed_bytes=$headerChanged;nonROI_changed_bytes=$outsideChanged;changed_bytes=$changed;label_count=$proofs.Count;labels=$proofs;method='GDI crop/resample only; white-core times actual alpha cutoff64/remap; retain original bright-core color and baseline';protected='All pixels outside approved Japanese text rectangles, header, standalone dash. L3/SP/1/3 retained verbatim in generated Korean labels.';limitations=@('This is an offline candidate, not a gameplay screenshot.','Blue-background binding defect is independent and not fixed by this asset.','Charge shot is original atlas text with no own sprite in this ARK; its original isolated physical ROI is translated but no new reference is created.','Faint original alpha outside approved source rectangles is preserved byte-exact.')}|ConvertTo-Json -Depth 12|Set-Content (Join-Path $out 'controller27-composition-manifest.json') -Encoding utf8
Write-Output "controller27 candidate SHA $(SHA $result), header/nonROI $headerChanged/$outsideChanged"
