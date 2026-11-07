$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
$folder=Join-Path (Get-Location) 'localization/en-US/images/combat-banner-v2'
New-Item -ItemType Directory -Force -Path $folder|Out-Null
$original=Join-Path (Get-Location) 'native/analysis/combat-caution-ui/ope60_title.dds'
$raw=[IO.File]::ReadAllBytes($original)
if((Get-FileHash -LiteralPath $original -Algorithm SHA256).Hash.ToLowerInvariant() -ne '2f2d5f14f687a822bf183abef14c4c245083628d90413ca77f54eddf51e314f5'){throw 'Original atlas changed'}
$ark=[IO.File]::ReadAllBytes((Join-Path (Get-Location) 'analysis/ui_assets/ope60.ark'))
$mask=[Drawing.Bitmap]::new(512,512,[Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g=[Drawing.Graphics]::FromImage($mask)
$g.Clear([Drawing.Color]::Transparent)
$g.TextRenderingHint=[Drawing.Text.TextRenderingHint]::AntiAliasGridFit
$format=[Drawing.StringFormat]::GenericTypographic.Clone()
$format.Alignment=[Drawing.StringAlignment]::Center
$format.LineAlignment=[Drawing.StringAlignment]::Center
$format.FormatFlags=$format.FormatFlags -bor [Drawing.StringFormatFlags]::NoWrap
$segments=@('BAT','TLE','STA','RT','FIN','ISH','MIS','SION','OBJ','ECT','IVE','MET','FAI','LED','CONT','INUE')
$proof=@()
for($i=0;$i -lt 16;$i++){
 if($i -lt 14){
  $offset=160+(6+$i)*48
  $rect=@([BitConverter]::ToInt32($ark,$offset+12),[BitConverter]::ToInt32($ark,$offset+16),[BitConverter]::ToInt32($ark,$offset+20),[BitConverter]::ToInt32($ark,$offset+24))
 }else{$rect=@(@(6,129,254,378)[$i%4],292,115,95)}
 if($rect[2] -ne 115 -or $rect[3] -ne 95){throw 'Unexpected original TXOS dimensions'}
 $fontSize=if($i -lt 4 -or ($i -ge 8 -and $i -lt 12)){42}else{38}
 if($i -ge 14){$fontSize=34}
 $font=[Drawing.Font]::new('Arial',[single]$fontSize,[Drawing.FontStyle]::Bold,[Drawing.GraphicsUnit]::Pixel)
 try{
  $measure=$g.MeasureString($segments[$i],$font,1000,$format)
  if($measure.Width -gt 105){throw "Segment exceeds protected sprite width: $($segments[$i])"}
  $g.DrawString($segments[$i],$font,[Drawing.Brushes]::White,[Drawing.RectangleF]::new($rect[0]+5,$rect[1]+8,105,76),$format)
 }finally{$font.Dispose()}
 $proof+=@{index=$i;text=$segments[$i];txos_xywh=$rect;font_pixels=$fontSize;source=if($i -lt 14){'ope60.ark TXOS'}else{'matching final atlas cells'}}
}
$g.Dispose();$format.Dispose()
for($y=0;$y -lt 384;$y++){for($x=0;$x -lt 512;$x++){$raw[128+($y*512+$x)*4+3]=$mask.GetPixel($x,$y).A}}
$output=Join-Path $folder 'ope60_title.dds'
[IO.File]::WriteAllBytes($output,$raw)
$mask.Save((Join-Path $folder 'atlas.png'),[Drawing.Imaging.ImageFormat]::Png)
$review=[Drawing.Bitmap]::new(520,440)
$rg=[Drawing.Graphics]::FromImage($review)
$rg.Clear([Drawing.Color]::FromArgb(12,22,25))
for($i=0;$i -lt 16;$i++){
 $r=$proof[$i].txos_xywh
 $rg.DrawImage($mask,[Drawing.Rectangle]::new((20+($i%4)*115),(10+[int][Math]::Floor($i/4)*108),115,95),$r[0],$r[1],$r[2],$r[3],[Drawing.GraphicsUnit]::Pixel)
}
$review.Save((Join-Path $folder 'assembled-review.png'),[Drawing.Imaging.ImageFormat]::Png)
$rg.Dispose();$review.Dispose();$mask.Dispose()
@{name='ope60_title.dds';source_sha256='2f2d5f14f687a822bf183abef14c4c245083628d90413ca77f54eddf51e314f5';replacement=$output;segments=$proof;live_review='pending'}|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $folder 'manifest.json') -Encoding utf8
