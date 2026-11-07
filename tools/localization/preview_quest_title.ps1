param([string]$Source,[string]$Output)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
$image=[System.Drawing.Bitmap]::new([string](Resolve-Path $Source).Path)
$bitmap=[System.Drawing.Bitmap]::new($image.Width*4,$image.Height*4)
$graphics=[System.Drawing.Graphics]::FromImage($bitmap)
$graphics.Clear([System.Drawing.Color]::Black)
$graphics.InterpolationMode=[System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
$graphics.DrawImage($image,0,0,$bitmap.Width,$bitmap.Height)
$bitmap.Save([System.IO.Path]::GetFullPath($Output),[System.Drawing.Imaging.ImageFormat]::Png)
$graphics.Dispose();$bitmap.Dispose();$image.Dispose()
