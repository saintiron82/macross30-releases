param([Parameter(Mandatory=$true)][string]$Spec)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
foreach($sheet in (Get-Content -LiteralPath $Spec -Raw -Encoding utf8|ConvertFrom-Json).sheets){
 $bitmap=[Drawing.Bitmap]::new([int]$sheet.width,[int]$sheet.height,[Drawing.Imaging.PixelFormat]::Format32bppArgb)
 $g=[Drawing.Graphics]::FromImage($bitmap)
 $g.Clear([Drawing.Color]::Transparent)
 $g.TextRenderingHint=[Drawing.Text.TextRenderingHint]::AntiAliasGridFit
 $format=[Drawing.StringFormat]::new();$format.Alignment=[Drawing.StringAlignment]::Center;$format.LineAlignment=[Drawing.StringAlignment]::Center
 foreach($row in $sheet.rows){
  $size=[Math]::Min(44,([int]$row.bottom-[int]$row.top)*0.80)
  do {
   $font=[Drawing.Font]::new('Arial',[single]$size,[Drawing.FontStyle]::Bold,[Drawing.GraphicsUnit]::Pixel)
   $measure=$g.MeasureString([string]$row.text,$font)
   if($measure.Width -le $sheet.width-16){break}
   $font.Dispose();$size-=1
  } while($size -gt 8)
  $g.DrawString([string]$row.text,$font,[Drawing.Brushes]::White,[Drawing.RectangleF]::new(0,[single]$row.top,[single]$sheet.width,[single]($row.bottom-$row.top)),$format)
  $font.Dispose()
 }
 $bitmap.Save([IO.Path]::GetFullPath($sheet.path),[Drawing.Imaging.ImageFormat]::Png)
 $format.Dispose();$g.Dispose();$bitmap.Dispose()
}
