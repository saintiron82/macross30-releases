$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing.Common,System.Drawing.Primitives,System.Private.Windows.GdiPlus,System.Private.Windows.Core -TypeDefinition @'
using System;using System.IO;using System.Drawing;using System.Drawing.Imaging;using System.Drawing.Drawing2D;using System.Runtime.InteropServices;
public static class EnglishTitle {
 static Bitmap Load(byte[] data,int w,int h){var b=new Bitmap(w,h,PixelFormat.Format32bppArgb);var bits=b.LockBits(new Rectangle(0,0,w,h),ImageLockMode.WriteOnly,b.PixelFormat);Marshal.Copy(data,128,bits.Scan0,w*h*4);b.UnlockBits(bits);return b;}
 static void Save(Bitmap b,byte[] original,string output,int rows){var data=(byte[])original.Clone();var bits=b.LockBits(new Rectangle(0,0,b.Width,b.Height),ImageLockMode.ReadOnly,b.PixelFormat);byte[] pixels=new byte[b.Width*b.Height*4];Marshal.Copy(bits.Scan0,pixels,0,pixels.Length);b.UnlockBits(bits);Array.Copy(pixels,0,data,128,b.Width*rows*4);File.WriteAllBytes(output+".dds",data);b.Save(output+".png",ImageFormat.Png);}
 public static void Build(string source,string generated,string output){
 byte[] logo=File.ReadAllBytes(Path.Combine(source,"title_logo02.dds"));
 if(BitConverter.ToInt32(logo,16)!=384||BitConverter.ToInt32(logo,12)!=64||logo.Length!=128+384*64*4)throw new Exception("Unexpected logo source");
 using(var b=Load(logo,384,64))using(var image=new Bitmap(generated)){
 int l=image.Width,t=image.Height,r=-1,bb=-1;for(int y=0;y<image.Height;y++)for(int x=0;x<image.Width;x++)if(image.GetPixel(x,y).A>=32){l=Math.Min(l,x);r=Math.Max(r,x);t=Math.Min(t,y);bb=Math.Max(bb,y);}if(r<l)throw new Exception("Empty alpha");
 var crop=Rectangle.FromLTRB(Math.Max(0,l-3),Math.Max(0,t-3),Math.Min(image.Width,r+4),Math.Min(image.Height,bb+4));
 using(var g=Graphics.FromImage(b)){g.CompositingMode=CompositingMode.SourceCopy;g.Clear(Color.Transparent);g.InterpolationMode=InterpolationMode.HighQualityBicubic;g.PixelOffsetMode=PixelOffsetMode.HighQuality;double scale=Math.Min(376.0/crop.Width,56.0/crop.Height);int w=(int)(crop.Width*scale),h=(int)(crop.Height*scale);g.DrawImage(image,new Rectangle((384-w)/2,(64-h)/2,w,h),crop,GraphicsUnit.Pixel);}
 Save(b,logo,Path.Combine(output,"title_logo02"),64);
 }
 byte[] menu=File.ReadAllBytes(Path.Combine(source,"title_text.dds"));
 if(menu.Length!=128+256*256*4)throw new Exception("Unexpected menu source");
 using(var b=Load(menu,256,256)){
 using(var g=Graphics.FromImage(b))using(var font=new Font("Arial",21,FontStyle.Regular,GraphicsUnit.Pixel))using(var format=new StringFormat()){
 g.CompositingMode=CompositingMode.SourceCopy;g.FillRectangle(Brushes.Transparent,0,0,256,144);g.CompositingMode=CompositingMode.SourceOver;g.TextRenderingHint=System.Drawing.Text.TextRenderingHint.AntiAliasGridFit;format.Alignment=StringAlignment.Center;format.LineAlignment=StringAlignment.Center;
 string[] labels={"PRESS START","NEW GAME","LOAD GAME"};for(int i=0;i<labels.Length;i++)g.DrawString(labels[i],font,Brushes.White,new RectangleF(0,i*48,256,48),format);
 }Save(b,menu,Path.Combine(output,"title_text"),144);
 }
 }
}
'@
$out=Join-Path (Get-Location) 'localization/en-US/images/title-v1'
[EnglishTitle]::Build((Join-Path (Get-Location) 'analysis/localization/title-menu-v11-audit/nested'),(Join-Path $out 'subtitle-generated.png'),$out)
Write-Output 'English title subtitle and three menu sprites built; DDS headers and protected bottom cells preserved.'
