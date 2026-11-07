param([string]$Out='localization/image_assets/title-menu-v11/candidate')
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing.Common,System.Drawing.Primitives,System.Private.Windows.GdiPlus,System.Private.Windows.Core -TypeDefinition @'
using System;using System.IO;using System.Drawing;using System.Drawing.Imaging;using System.Drawing.Drawing2D;using System.Runtime.InteropServices;
public static class TitleGlyphs {
 static Bitmap Glyph(Bitmap sheet,int y0,int y1){
  int l=sheet.Width,r=-1,t=y1,b=-1;
  for(int y=y0;y<y1;y++)for(int x=0;x<sheet.Width;x++)if(sheet.GetPixel(x,y).A>=96){l=Math.Min(l,x);r=Math.Max(r,x);t=Math.Min(t,y);b=Math.Max(b,y);}
  if(r<l)throw new Exception("Empty generated row");
  l=Math.Max(0,l-8);r=Math.Min(sheet.Width-1,r+8);t=Math.Max(y0,t-8);b=Math.Min(y1-1,b+8);
  return sheet.Clone(new Rectangle(l,t,r-l+1,b-t+1),PixelFormat.Format32bppArgb);
 }
 public static void Build(string source,string sheetFile,string output,bool menu){
  byte[] old=File.ReadAllBytes(source),raw=(byte[])old.Clone();int w=BitConverter.ToInt32(old,16),h=BitConverter.ToInt32(old,12);
  if(old.Length!=128+w*h*4||BitConverter.ToInt32(old,88)!=32)throw new Exception("Expected BGRA8 DDS");
  byte[] mask=new byte[w*h];
  using(var atlas=new Bitmap(w,h,PixelFormat.Format32bppArgb))using(var sheet=new Bitmap(sheetFile)){
   var bits=atlas.LockBits(new Rectangle(0,0,w,h),ImageLockMode.WriteOnly,PixelFormat.Format32bppArgb);Marshal.Copy(old,128,bits.Scan0,w*h*4);atlas.UnlockBits(bits);
   int[][] specs=menu?new[]{new[]{520,644,0,0,256,48},new[]{0,156,0,48,256,48},new[]{156,276,0,96,256,48}}:new[]{new[]{644,820,0,0,384,64}};
   foreach(var s in specs){using(var glyph=Glyph(sheet,s[0],s[1])){
    var rect=new Rectangle(s[2],s[3],s[4],s[5]);double scale=Math.Min((rect.Width-12.0)/glyph.Width,(rect.Height-8.0)/glyph.Height);int dw=(int)Math.Round(glyph.Width*scale),dh=(int)Math.Round(glyph.Height*scale);
    using(var g=Graphics.FromImage(atlas)){g.CompositingMode=CompositingMode.SourceCopy;g.FillRectangle(Brushes.Transparent,rect);g.InterpolationMode=InterpolationMode.HighQualityBicubic;g.PixelOffsetMode=PixelOffsetMode.HighQuality;g.DrawImage(glyph,new Rectangle(rect.X+(rect.Width-dw)/2,rect.Y+(rect.Height-dh)/2,dw,dh));}
    for(int y=rect.Y;y<rect.Bottom;y++)for(int x=rect.X;x<rect.Right;x++)mask[y*w+x]=1;
   }}
   bits=atlas.LockBits(new Rectangle(0,0,w,h),ImageLockMode.ReadOnly,PixelFormat.Format32bppArgb);byte[] pixels=new byte[w*h*4];Marshal.Copy(bits.Scan0,pixels,0,pixels.Length);atlas.UnlockBits(bits);
   for(int i=0;i<mask.Length;i++)if(mask[i]!=0)Array.Copy(pixels,i*4,raw,128+i*4,4);
   File.WriteAllBytes(output+".dds",raw);File.WriteAllBytes(output+".mask",mask);
   atlas.Save(output+".png",ImageFormat.Png);
   using(var preview=new Bitmap(w*3,h*3)){using(var g=Graphics.FromImage(preview)){g.Clear(Color.FromArgb(12,35,60));g.InterpolationMode=InterpolationMode.NearestNeighbor;g.DrawImage(atlas,new Rectangle(0,0,w*3,h*3));}preview.Save(output+"-preview3x.png",ImageFormat.Png);}
  }
 }
}
'@
if(Test-Path -LiteralPath $Out){throw 'Fresh candidate directory required'}
New-Item -ItemType Directory -Force $Out | Out-Null
$src=Join-Path (Get-Location) 'analysis/localization/title-menu-v11-audit/nested'
$sheet=(Resolve-Path 'analysis/localization/title-menu-v11-audit/generated-korean-sheet.png').Path
foreach($name in @('title_text','title_logo02')){
 [TitleGlyphs]::Build((Join-Path $src ($name+'.dds')),$sheet,(Join-Path (Join-Path (Get-Location) $Out) $name),($name -eq 'title_text'))
}
Write-Output 'Generated glyphs composed into exact title sprite cells; source files untouched'
