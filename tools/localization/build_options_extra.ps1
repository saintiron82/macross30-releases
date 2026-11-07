param([string]$Root=(Resolve-Path "$PSScriptRoot/../..").Path)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
$bundle=Join-Path $Root 'localization/image_assets/menu-ui-v7'
$out=Join-Path $bundle 'replacements';New-Item -ItemType Directory -Force $out|Out-Null
$audit=Join-Path $bundle 'audit'
function SHA([byte[]]$b){[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($b)).ToLowerInvariant()}
$sheetPath=Join-Path $bundle 'generated/options-9.png';$sheet=[Drawing.Bitmap]::new([string]$sheetPath)
if($sheet.Width -ne 1024 -or $sheet.Height -ne 1536){throw 'Unexpected source sheet size'}
$lock=$sheet.LockBits([Drawing.Rectangle]::new(0,0,$sheet.Width,$sheet.Height),[Drawing.Imaging.ImageLockMode]::ReadOnly,[Drawing.Imaging.PixelFormat]::Format32bppArgb)
[byte[]]$source=[byte[]]::new($lock.Stride*$sheet.Height);[Runtime.InteropServices.Marshal]::Copy($lock.Scan0,$source,0,$source.Length);$sheet.UnlockBits($lock)
$states=@{}
foreach($n in @('opt_text00','opt_logo00','opt_brighttext')){
 [byte[]]$dds=[IO.File]::ReadAllBytes((Join-Path $Root "analysis/assets/data/$n.dds"));$w=[BitConverter]::ToInt32($dds,16);$h=[BitConverter]::ToInt32($dds,12)
 [byte[]]$pixels=if($n -eq 'opt_text00'){[byte[]]$dds[128..($dds.Length-1)]}else{[IO.File]::ReadAllBytes((Join-Path $audit "$n.bgra"))}
 if($pixels.Length -ne $w*$h*4){throw 'Invalid decoded source size'}
 $states[$n]=@{header=[byte[]]$dds[0..127];original=$pixels;pixels=([byte[]]$pixels.Clone());width=$w;height=$h;allowed=([bool[]]::new($w*$h));proofs=@();source_sha256=(SHA $dds)}
}
$six=(Get-Content (Join-Path $audit 'options6-final-rois.json') -Raw|ConvertFrom-Json).labels
$specs=@();$bands=@(@(71,175),@(243,348),@(422,529),@(600,705),@(788,895),@(971,1074),@(1152,1255),@(1331,1372),@(1431,1471))
for($n=0;$n -lt 6;$n++){$specs+=@{asset='opt_text00';Japanese=$six[$n].source;Korean=$six[$n].target;roi=$six[$n].candidate_safe_ROI;box=$six[$n].source_visible_bbox_alpha_ge64;center=$false}}
$specs+=@{asset='opt_logo00';Japanese='オプション';Korean='옵션';roi=@(4,8,188,56);box=@(15,16,176,48);center=$true}
$specs+=@{asset='opt_brighttext';Japanese='うっすらとS.M.Sマークが見える設定が推奨明度になります。';Korean='S.M.S 마크가 희미하게 보일 정도가 권장 밝기입니다.';roi=@(0,0,512,24);box=@(36,6,475,22);center=$true}
$specs+=@{asset='opt_brighttext';Japanese='ご使用のモニターの見え方に合わせて調整してください。';Korean='사용 중인 모니터에서 보이는 상태에 맞춰 조정해 주세요.';roi=@(0,24,512,48);box=@(54,26,458,42);center=$true}
for($n=0;$n -lt $specs.Count;$n++){
 $spec=$specs[$n];$s=$states[$spec.asset];$band=$bands[$n];$left=1024;$right=-1;$top=1536;$bottom=-1
 for($y=$band[0];$y -lt $band[1];$y++){for($x=0;$x -lt 1024;$x++){
  $i=4*($y*1024+$x);$core=$source[$i+3]*[Math]::Min($source[$i],[Math]::Min($source[$i+1],$source[$i+2]))/255.0
  if($core -ge 64){$left=[Math]::Min($left,$x);$right=[Math]::Max($right,$x);$top=[Math]::Min($top,$y);$bottom=[Math]::Max($bottom,$y)}
 }}
 if($right -lt $left){throw 'Empty glyph row'};$cw=$right-$left+1;$ch=$bottom-$top+1
 $clean=[Drawing.Bitmap]::new($cw,$ch,[Drawing.Imaging.PixelFormat]::Format32bppArgb)
 for($y=0;$y -lt $ch;$y++){for($x=0;$x -lt $cw;$x++){
  $i=4*(($top+$y)*1024+$left+$x);$core=$source[$i+3]*[Math]::Min($source[$i],[Math]::Min($source[$i+1],$source[$i+2]))/255.0
  $a=if($core -lt 64){0}else{[int][Math]::Round(($core-64)*255/191)};$clean.SetPixel($x,$y,[Drawing.Color]::FromArgb($a,255,255,255))
 }}
 $r=$spec.roi;$box=$spec.box;$height=[int]$box[3]-[int]$box[1];$width=[Math]::Min(($r[2]-$r[0]-4),[int][Math]::Round($cw*$height/[double]$ch))
 $dx=if($spec.center){[int][Math]::Floor(($r[0]+$r[2]-$width)/2.0)}else{[Math]::Max($r[0]+1,$box[0])};$dy=[int]$box[1]
 $width=[Math]::Min($width,($r[2]-$dx-1))
 if($dx+$width -gt $r[2] -or $dy+$height -gt $r[3]){throw 'Destination outside safe ROI'}
 $small=[Drawing.Bitmap]::new($width,$height,[Drawing.Imaging.PixelFormat]::Format32bppArgb);$g=[Drawing.Graphics]::FromImage($small)
 $g.CompositingMode=[Drawing.Drawing2D.CompositingMode]::SourceCopy;$g.InterpolationMode=[Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic;$g.PixelOffsetMode=[Drawing.Drawing2D.PixelOffsetMode]::HighQuality
 $g.DrawImage($clean,[Drawing.Rectangle]::new(0,0,$width,$height));$g.Dispose();$clean.Dispose()
 $hist=@{}
 for($y=$r[1];$y -lt $r[3];$y++){for($x=$r[0];$x -lt $r[2];$x++){$i=4*($y*$s.width+$x);if($s.original[$i+3] -ge 180){$key="$($s.original[$i]),$($s.original[$i+1]),$($s.original[$i+2])";$hist[$key]++}}}
 $bright=0;foreach($key in $hist.Keys){$p=$key.Split(',');$bright=[Math]::Max($bright,[Math]::Max([int]$p[0],[Math]::Max([int]$p[1],[int]$p[2])))}
 $color=($hist.GetEnumerator()|Where-Object{$p=$_.Name.Split(',');[Math]::Max([int]$p[0],[Math]::Max([int]$p[1],[int]$p[2])) -ge $bright-8}|Sort-Object Value -Descending|Select-Object -First 1).Name
 if(!$color){throw 'No original core color'};$bgr=@($color.Split(',')|ForEach-Object{[byte]$_})
 for($y=$r[1];$y -lt $r[3];$y++){for($x=$r[0];$x -lt $r[2];$x++){$pi=$y*$s.width+$x;if($s.allowed[$pi]){throw 'Overlap'};$s.allowed[$pi]=$true;$s.pixels[4*$pi+3]=0}}
 $visible=0
 for($y=0;$y -lt $height;$y++){for($x=0;$x -lt $width;$x++){$a=$small.GetPixel($x,$y).A;if(!$a){continue};$i=4*(($dy+$y)*$s.width+$dx+$x);$s.pixels[$i]=$bgr[0];$s.pixels[$i+1]=$bgr[1];$s.pixels[$i+2]=$bgr[2];$s.pixels[$i+3]=$a;$visible++}}
 $small.Dispose();if(!$visible){throw 'Invisible result'}
 $s.proofs+=@{Japanese=$spec.Japanese;Korean=$spec.Korean;ROI_ltrb=$r;source_crop_xywh=@($left,$top,$cw,$ch);destination_xywh=@($dx,$dy,$width,$height);original_BGR=$bgr;visible_pixels=$visible}
}
$results=@()
foreach($name in @('opt_text00','opt_logo00','opt_brighttext')){
 $s=$states[$name];$outside=0;$changed=0
 for($i=0;$i -lt $s.original.Length;$i++){if($s.original[$i] -ne $s.pixels[$i]){$changed++;if(!$s.allowed[[int][Math]::Floor($i/4)]){$outside++}}}
 if($outside){throw 'NonROI pixel edit'}
 if($name -eq 'opt_text00'){[IO.File]::WriteAllBytes((Join-Path $out "$name.dds"),[byte[]]($s.header+$s.pixels))}else{[IO.File]::WriteAllBytes((Join-Path $out "$name.composited.bgra"),$s.pixels)}
 $im=[Drawing.Bitmap]::new($s.width,$s.height,[Drawing.Imaging.PixelFormat]::Format32bppArgb);$l=$im.LockBits([Drawing.Rectangle]::new(0,0,$s.width,$s.height),[Drawing.Imaging.ImageLockMode]::WriteOnly,[Drawing.Imaging.PixelFormat]::Format32bppArgb)
 [Runtime.InteropServices.Marshal]::Copy($s.pixels,0,$l.Scan0,$s.pixels.Length);$im.UnlockBits($l);$im.Save((Join-Path $out "$name.png"))
 $black=[Drawing.Bitmap]::new(($s.width*4),($s.height*4));$g=[Drawing.Graphics]::FromImage($black);$g.Clear([Drawing.Color]::Black);$g.InterpolationMode=[Drawing.Drawing2D.InterpolationMode]::NearestNeighbor;$g.PixelOffsetMode=[Drawing.Drawing2D.PixelOffsetMode]::Half;$g.DrawImage($im,0,0,$black.Width,$black.Height);$black.Save((Join-Path $out "$name-black4x.png"));$g.Dispose();$black.Dispose();$im.Dispose()
 $results+=@{asset=$name;source_sha256=$s.source_sha256;pixel_changed_bytes=$changed;nonROI_pixel_changed_bytes=$outside;labels=$s.proofs}
}
$sheet.Dispose()
@{status='OFFLINE_GDI_COMPOSITION_BC3_REENCODE_PENDING';glyph_sheet_sha256=(SHA ([IO.File]::ReadAllBytes($sheetPath)));method='Actual alpha times white core, cutoff64/remap; GDI crop/resample only; preserve source color/baseline';results=$results}|ConvertTo-Json -Depth 10|Set-Content (Join-Path $out 'options9-composition-manifest.json') -Encoding utf8
Write-Output 'options9 GDI composition complete; run encode_options_ui.py for BC3 byte assets and verification'
