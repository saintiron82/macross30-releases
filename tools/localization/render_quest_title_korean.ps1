param([string]$Glyphs,[string]$Original,[string]$Output)
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;using System.IO;using System.Drawing;using System.Drawing.Imaging;using System.Drawing.Drawing2D;
public static class QuestTitleGdi {
 public static void Run(string glyph,string original,string output){
 using(var src=new Bitmap(glyph))using(var basis=new Bitmap(original))using(var mask=new Bitmap(src.Width,src.Height,PixelFormat.Format32bppArgb)){
 int[] counts=new int[src.Height];
 for(int y=0;y<src.Height;y++)for(int x=0;x<src.Width;x++){
 var c=src.GetPixel(x,y);int white=Math.Min(c.R,Math.Min(c.G,c.B));int a=white*c.A/255;
 if(a<64)a=0;mask.SetPixel(x,y,Color.FromArgb(a,255,255,255));if(a>=192)counts[y]++;
 }
 // Actual first glyph band: alpha>=192 core, at least 30 occupied pixels.
 int top=-1,bottom=-1;for(int y=0;y<src.Height;y++){if(counts[y]>=30){if(top<0)top=y;bottom=y+1;}else if(top>=0&&y-bottom>=4)break;}
 if(top<0||bottom-top<15||bottom-top>180)throw new Exception("First glyph band not isolated");
 int left=src.Width,right=0;for(int y=top;y<bottom;y++)for(int x=0;x<src.Width;x++)if(mask.GetPixel(x,y).A>=192){left=Math.Min(left,x);right=Math.Max(right,x+1);}
 if(right-left<80||right-left>900)throw new Exception("Invalid first glyph width");
 using(var result=new Bitmap(basis.Width,basis.Height,PixelFormat.Format32bppArgb))using(var g=Graphics.FromImage(result)){
 g.CompositingMode=CompositingMode.SourceCopy;g.DrawImageUnscaled(basis,0,0);
 // The orange plate is part of this exact title DDS, not quest_start assets.
 var orange=basis.GetPixel(180,12);using(var brush=new SolidBrush(orange))g.FillRectangle(brush,176,10,293,75);
 g.CompositingMode=CompositingMode.SourceOver;g.InterpolationMode=InterpolationMode.HighQualityBicubic;
 float scale=Math.Min(271f/(right-left),61f/(bottom-top));int w=(int)Math.Round((right-left)*scale),h=(int)Math.Round((bottom-top)*scale);
 g.DrawImage(mask,new Rectangle(176+(293-w)/2,10+(75-h)/2,w,h),new Rectangle(left,top,right-left,bottom-top),GraphicsUnit.Pixel);
 Directory.CreateDirectory(output);result.Save(Path.Combine(output,"quest_title_01_02.png"),ImageFormat.Png);
 using(var f=new BinaryWriter(File.Create(Path.Combine(output,"quest_title_01_02.rgba"))))for(int y=0;y<result.Height;y++)for(int x=0;x<result.Width;x++){var c=result.GetPixel(x,y);f.Write(c.R);f.Write(c.G);f.Write(c.B);f.Write(c.A);}
 File.WriteAllText(Path.Combine(output,"gdi-title-roi.json"),"{\"source_xyxy\":["+left+","+top+","+right+","+bottom+"],\"destination_xyxy\":[176,10,469,85],\"alpha_rule\":\"minRGB*alpha/255; below64 zero; row band uses core>=192\",\"compositor\":\"System.Drawing/GDI+\"}");
 }
 }}
}
'@
[QuestTitleGdi]::Run($Glyphs,$Original,$Output)
