param([string]$Out='localization/image_assets/title-menu-v12/candidate')
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing.Common,System.Drawing.Primitives,System.Private.Windows.GdiPlus,System.Private.Windows.Core -TypeDefinition @'
using System;using System.IO;using System.Drawing;using System.Drawing.Imaging;using System.Drawing.Drawing2D;using System.Runtime.InteropServices;
public static class TitleRefit12 {
 static Rectangle Ink(Bitmap b,Rectangle r,int threshold){int l=r.Right,t=r.Bottom,rr=-1,bb=-1;for(int y=r.Y;y<r.Bottom;y++)for(int x=r.X;x<r.Right;x++)if(b.GetPixel(x,y).A>threshold){l=Math.Min(l,x);t=Math.Min(t,y);rr=Math.Max(rr,x);bb=Math.Max(bb,y);}if(rr<l)throw new Exception("Empty ink");return Rectangle.FromLTRB(l,t,rr+1,bb+1);}
 public static void Build(string source,string sheetFile,string output){
  byte[] old=File.ReadAllBytes(source),raw=(byte[])old.Clone();int w=BitConverter.ToInt32(old,16),h=BitConverter.ToInt32(old,12);
  if(w!=256||h!=256||old.Length!=128+w*h*4||BitConverter.ToInt32(old,88)!=32)throw new Exception("Expected title BGRA8");byte[] mask=new byte[w*h];
  using(var atlas=new Bitmap(w,h,PixelFormat.Format32bppArgb))using(var sheet=new Bitmap(sheetFile)){
   var bits=atlas.LockBits(new Rectangle(0,0,w,h),ImageLockMode.WriteOnly,PixelFormat.Format32bppArgb);Marshal.Copy(old,128,bits.Scan0,w*h*4);atlas.UnlockBits(bits);
   int[][] rows={new[]{520,644,0},new[]{0,156,48},new[]{156,276,96}};
   foreach(var row in rows){Rectangle core=Ink(sheet,new Rectangle(0,row[0],sheet.Width,row[1]-row[0]),96);Rectangle crop=Rectangle.FromLTRB(Math.Max(0,core.Left-8),Math.Max(row[0],core.Top-8),Math.Min(sheet.Width,core.Right+8),Math.Min(row[1],core.Bottom+8));
    using(var glyph=sheet.Clone(crop,PixelFormat.Format32bppArgb)){
     var cell=new Rectangle(0,row[2],256,48);double scale=Math.Min(22.0/core.Height,244.0/crop.Width);bool fits=false;
     for(int attempt=0;attempt<8;attempt++){
      int dw=(int)Math.Round(crop.Width*scale),dh=(int)Math.Round(crop.Height*scale);
      using(var g=Graphics.FromImage(atlas)){g.CompositingMode=CompositingMode.SourceCopy;g.FillRectangle(Brushes.Transparent,cell);g.InterpolationMode=InterpolationMode.HighQualityBicubic;g.PixelOffsetMode=PixelOffsetMode.HighQuality;g.DrawImage(glyph,new Rectangle((256-dw)/2,row[2]+(48-dh)/2,dw,dh));}
      Rectangle ink=Ink(atlas,cell,96);if(ink.Height<=22&&ink.Width<=244){fits=true;break;}scale*=0.95;
     }if(!fits)throw new Exception("Refitted ink exceeds original+2px");
     for(int y=cell.Top;y<cell.Bottom;y++)for(int x=0;x<256;x++)mask[y*w+x]=1;
    }
   }
   bits=atlas.LockBits(new Rectangle(0,0,w,h),ImageLockMode.ReadOnly,PixelFormat.Format32bppArgb);byte[] pixels=new byte[w*h*4];Marshal.Copy(bits.Scan0,pixels,0,pixels.Length);atlas.UnlockBits(bits);for(int i=0;i<mask.Length;i++)if(mask[i]!=0)Array.Copy(pixels,i*4,raw,128+i*4,4);
   File.WriteAllBytes(output+".dds",raw);File.WriteAllBytes(output+".mask",mask);atlas.Save(output+".png",ImageFormat.Png);
   using(var preview=new Bitmap(w*3,144*3)){using(var g=Graphics.FromImage(preview)){g.Clear(Color.FromArgb(12,35,60));g.InterpolationMode=InterpolationMode.NearestNeighbor;g.DrawImage(atlas,new Rectangle(0,0,w*3,h*3));}preview.Save(output+"-preview3x.png",ImageFormat.Png);}
  }
 }
}
'@
if(Test-Path -LiteralPath $Out){throw 'Fresh candidate directory required'}
$root=(Get-Location).Path
$src=Join-Path $root 'localization/image_assets/title-menu-v11/candidate/title_text.dds'
if((Get-FileHash -LiteralPath $src -Algorithm SHA256).Hash.ToLower() -ne 'b18fd3e78660efd8e29566f662d3c528b2989e4c8f31409a20f20951d7abbf96'){throw 'v11 title source changed'}
New-Item -ItemType Directory -Force $Out | Out-Null
$sheet=(Resolve-Path 'analysis/localization/title-menu-v11-audit/generated-korean-sheet.png').Path
[TitleRefit12]::Build($src,$sheet,(Join-Path (Join-Path $root $Out) 'title_text'))
$sourceDir=Join-Path $root 'localization/image_assets/title-menu-v12/source'
New-Item -ItemType Directory -Force $sourceDir | Out-Null
Copy-Item -LiteralPath $src -Destination (Join-Path $sourceDir 'title_text.dds')
$dds=Join-Path (Join-Path $root $Out) 'title_text.dds';$mask=Join-Path (Join-Path $root $Out) 'title_text.mask'
$entry=[ordered]@{name='title_text.dds';path='data/menu/title/title_text.dds';source_DDS=(Join-Path $sourceDir 'title_text.dds');source_sha256='b18fd3e78660efd8e29566f662d3c528b2989e4c8f31409a20f20951d7abbf96';replacement=$dds;replacement_sha256=(Get-FileHash -LiteralPath $dds -Algorithm SHA256).Hash.ToLower();allowed_pixel_mask=$mask;allowed_mask_sha256=(Get-FileHash -LiteralPath $mask -Algorithm SHA256).Hash.ToLower();ROI_xywh=@(@(0,0,256,48),@(0,48,256,48),@(0,96,256,48));protected_xywh=,@(0,144,256,112);labels=@('START 버튼을 누르세요','게임 시작','불러오기')}
$doc=[ordered]@{status='OFFLINE_CANDIDATE';entries=@($entry);glyph_source=$sheet;core_alpha_threshold=96;max_core_ink_height=22;reason='Original core20px, v11 menu34px exceeds native approximate32px row pitch; refit to original+2px';installed=$false;in_game_verified=$false;protected='Everything except title_text three cells, including v11 subtitle and original brand/background/ARK'}
$doc | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $Out 'authority.json') -Encoding utf8
Write-Output 'Prepared v12: existing Korean glyphs refit to coreheight <=22px, active assets untouched'
