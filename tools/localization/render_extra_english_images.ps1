param([Parameter(Mandatory=$true)][string]$Plan)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing.Common,System.Drawing.Primitives,System.Private.Windows.GdiPlus,System.Private.Windows.Core,System.Collections -TypeDefinition @'
using System;using System.IO;using System.Drawing;using System.Drawing.Imaging;using System.Drawing.Text;using System.Runtime.InteropServices;
public static class ExtraEnglishText {
 public static Bitmap Load(string path,int w,int h){var b=new Bitmap(w,h,PixelFormat.Format32bppArgb);byte[] raw=File.ReadAllBytes(path);if(raw.Length!=w*h*4)throw new Exception("BGRA length");var l=b.LockBits(new Rectangle(0,0,w,h),ImageLockMode.WriteOnly,PixelFormat.Format32bppArgb);Marshal.Copy(raw,0,l.Scan0,raw.Length);b.UnlockBits(l);return b;}
 public static void Erase(Bitmap b,Bitmap source,string mode,int x,int y,int w,int h){
  if(mode=="clear"){for(int yy=y;yy<y+h;yy++)for(int xx=x;xx<x+w;xx++)b.SetPixel(xx,yy,Color.Transparent);}
  if(mode=="panel"){for(int yy=y;yy<y+h;yy++)for(int xx=x;xx<x+w;xx++){var a=source.GetPixel(x-1,yy);var z=source.GetPixel(x+w,yy);float t=(xx-x+1f)/(w+1);b.SetPixel(xx,yy,Color.FromArgb((int)(a.A*(1-t)+z.A*t),(int)(a.R*(1-t)+z.R*t),(int)(a.G*(1-t)+z.G*t),(int)(a.B*(1-t)+z.B*t)));}}
 }
 public static void Text(Bitmap b,string text,int x,int y,int w,int h,float size,int r,int green,int blue){
  using(var g=Graphics.FromImage(b))using(var format=new StringFormat(StringFormat.GenericTypographic)){
   g.TextRenderingHint=TextRenderingHint.AntiAliasGridFit;format.Alignment=StringAlignment.Center;format.LineAlignment=StringAlignment.Center;format.FormatFlags|=StringFormatFlags.NoWrap;
   Font font=null;for(float s=Math.Min(size,h);s>=5;s-=0.5f){font=new Font("Arial",s,FontStyle.Bold,GraphicsUnit.Pixel);if(g.MeasureString(text,font,PointF.Empty,format).Width<=w-2)break;font.Dispose();font=null;}
   if(font==null)throw new Exception("Text cannot fit: "+text);
   using(font)using(var brush=new SolidBrush(Color.FromArgb(255,r,green,blue))){g.SetClip(new Rectangle(x,y,w,h));g.DrawString(text,font,brush,new RectangleF(x,y,w,h),format);}
  }
 }
 public static void Scan(Bitmap b,Bitmap original){b.RotateFlip(RotateFlipType.RotateNoneFlipX);for(int y=0;y<b.Height;y++){int alpha=0;for(int x=0;x<b.Width;x++)alpha=Math.Max(alpha,original.GetPixel(x,y).A);for(int x=0;x<b.Width;x++){var c=b.GetPixel(x,y);b.SetPixel(x,y,Color.FromArgb(c.A*alpha/255,c.R,c.G,c.B));}}}
 public static void Art(Bitmap b,string path,int x,int y,int w,int h,float feather){using(var edited=new Bitmap(path))using(var scaled=new Bitmap(b.Width,b.Height)){using(var g=Graphics.FromImage(scaled)){g.DrawImage(edited,new Rectangle(0,0,b.Width,b.Height));}for(int yy=y;yy<y+h;yy++)for(int xx=x;xx<x+w;xx++){float edge=Math.Min((x+w-1-xx)/feather,(y+h-1-yy)/feather);float t=Math.Min(1f,Math.Max(0f,edge));var a=b.GetPixel(xx,yy);var z=scaled.GetPixel(xx,yy);b.SetPixel(xx,yy,Color.FromArgb(a.A,(int)(a.R*(1-t)+z.R*t),(int)(a.G*(1-t)+z.G*t),(int)(a.B*(1-t)+z.B*t)));}}}
 public static void Mips(Bitmap b,string stem,int levels){for(int n=1;n<levels;n++){int w=Math.Max(1,b.Width>>n),h=Math.Max(1,b.Height>>n);using(var scaled=new Bitmap(w,h,PixelFormat.Format32bppArgb)){using(var g=Graphics.FromImage(scaled)){g.CompositingMode=System.Drawing.Drawing2D.CompositingMode.SourceCopy;g.InterpolationMode=System.Drawing.Drawing2D.InterpolationMode.HighQualityBilinear;g.DrawImage(b,new Rectangle(0,0,w,h));}Save(scaled,stem+".mip"+n);}}}
 public static void Save(Bitmap b,string stem){Directory.CreateDirectory(Path.GetDirectoryName(stem));b.Save(stem+".png",ImageFormat.Png);var l=b.LockBits(new Rectangle(0,0,b.Width,b.Height),ImageLockMode.ReadOnly,PixelFormat.Format32bppArgb);byte[] bgra=new byte[b.Width*b.Height*4];Marshal.Copy(l.Scan0,bgra,0,bgra.Length);b.UnlockBits(l);for(int i=0;i<bgra.Length;i+=4){byte c=bgra[i];bgra[i]=bgra[i+2];bgra[i+2]=c;}File.WriteAllBytes(stem+".rgba",bgra);using(var preview=new Bitmap(b.Width,b.Height))using(var g=Graphics.FromImage(preview)){g.Clear(Color.FromArgb(24,28,32));g.DrawImageUnscaled(b,0,0);preview.Save(stem+"-review.png",ImageFormat.Png);}}
}
'@
$doc=Get-Content -LiteralPath $Plan -Raw -Encoding utf8|ConvertFrom-Json
foreach($a in $doc.assets){
 $source=[ExtraEnglishText]::Load($a.source_BGRA,$a.width,$a.height)
 $bitmap=if($a.mode -in @('panel','art','clear')){$source.Clone()}else{[Drawing.Bitmap]::new([int]$a.width,[int]$a.height,[Drawing.Imaging.PixelFormat]::Format32bppArgb)}
 try {
  if($a.mode -eq 'art'){$r=$a.rows[0].rect;$feather=if($a.feather){[single]$a.feather}else{16.0};[ExtraEnglishText]::Art($bitmap,$a.generated_edit,$r[0],$r[1],$r[2],$r[3],$feather)}
  foreach($row in $a.rows){
   if($a.mode -eq 'art'){continue}
   $r=$row.rect
   [ExtraEnglishText]::Erase($bitmap,$source,$a.mode,$r[0],$r[1],$r[2],$r[3])
   $color=if($row.color){$row.color}else{$a.color}
   [ExtraEnglishText]::Text($bitmap,$row.text,$r[0],$r[1],$r[2],$r[3],$row.size,$color[0],$color[1],$color[2])
  }
  if($a.mode -eq 'scan'){[ExtraEnglishText]::Scan($bitmap,$source)}
  [ExtraEnglishText]::Save($bitmap,$a.output_stem)
  if($a.mip_levels -gt 1){[ExtraEnglishText]::Mips($bitmap,$a.output_stem,$a.mip_levels)}
 }finally{$bitmap.Dispose();$source.Dispose()}
}
