$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
$plan=Get-Content -LiteralPath localization/en-US/images/loading-names-v1/edit-plan.json -Raw -Encoding utf8|ConvertFrom-Json
foreach($row in $plan.entries){
 $image=[Drawing.Bitmap]::new([string]$row.source_png)
 try {
  $r=$row.roi_xywh
  $crop=$image.Clone([Drawing.Rectangle]::new($r[0],$r[1],$r[2],$r[3]),[Drawing.Imaging.PixelFormat]::Format32bppArgb)
  try {$crop.Save([string]$row.crop_png,[Drawing.Imaging.ImageFormat]::Png)}finally{$crop.Dispose()}
 }finally{$image.Dispose()}
}
