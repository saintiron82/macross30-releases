param(
 [string]$Glyphs='localization/image_assets/caution-ui-v4/source-glyphs.png',
 [string]$Original='native/analysis/combat-caution-ui/ope60_title.dds',
 [string]$Output='localization/image_assets/caution-ui-v4/replacements'
)
$ErrorActionPreference='Stop'
if ((Get-FileHash -LiteralPath $Original -Algorithm SHA256).Hash.ToLowerInvariant() -ne '2f2d5f14f687a822bf183abef14c4c245083628d90413ca77f54eddf51e314f5') { throw 'Original caution atlas SHA mismatch' }
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;using System.IO;using System.Text;using System.Drawing;using System.Drawing.Imaging;using System.Drawing.Drawing2D;
public static class CautionGlyphAtlas {
 public static void Run(string glyphs,string original,string output){
 byte[] source=File.ReadAllBytes(original), result=(byte[])source.Clone();
 if(source.Length!=1048704||Encoding.ASCII.GetString(source,0,4)!="DDS "||BitConverter.ToInt32(source,12)!=512||BitConverter.ToInt32(source,16)!=512||BitConverter.ToInt32(source,84)!=0||BitConverter.ToInt32(source,88)!=32)throw new Exception("Unexpected original BGRA32 DDS");
 for(int i=128;i<128+512*384*4;i+=4)if(source[i]!=255||source[i+1]!=255||source[i+2]!=255)throw new Exception("Original RGB not constant white");
 Directory.CreateDirectory(output);var meta=new StringBuilder("[ ");
 using(var input=new Bitmap(glyphs))using(var alpha=new Bitmap(512,512,PixelFormat.Format32bppArgb))using(var g=Graphics.FromImage(alpha)){
 if(input.Width!=1536||input.Height!=1024)throw new Exception("Expected original 4x4 source grid");
 g.Clear(Color.Transparent);g.InterpolationMode=InterpolationMode.HighQualityBicubic;g.CompositingMode=CompositingMode.SourceOver;
 for(int n=0;n<16;n++){
 int sx=(n%4)*384,sy=(n/4)*256,left=384,top=256,right=0,bottom=0;
 using(var mask=new Bitmap(384,256,PixelFormat.Format32bppArgb)){
 for(int y=0;y<256;y++)for(int x=0;x<384;x++){
 var c=input.GetPixel(sx+x,sy+y);int a=Math.Min(c.R,Math.Min(c.G,c.B))*c.A/255;if(a<64)a=0;
 mask.SetPixel(x,y,Color.FromArgb(a,255,255,255));
 if(a>=192){left=Math.Min(left,x);top=Math.Min(top,y);right=Math.Max(right,x+1);bottom=Math.Max(bottom,y+1);}
 }
 if(right-left<40||bottom-top<40||right-left>300||bottom-top>220)throw new Exception("Invalid glyph cell "+n);
 float scale=Math.Min(102f/(right-left),78f/(bottom-top));int w=(int)Math.Round((right-left)*scale),h=(int)Math.Round((bottom-top)*scale);
 int dx=(n%4)*128+(128-w)/2,dy=(n/4)*96+(96-h)/2;
 g.DrawImage(mask,new Rectangle(dx,dy,w,h),new Rectangle(left,top,right-left,bottom-top),GraphicsUnit.Pixel);
 if(n>0)meta.Append(",");meta.Append("{\"index\":"+n+",\"source_core_xyxy\":["+(sx+left)+","+(sy+top)+","+(sx+right)+","+(sy+bottom)+"],\"destination_core_xyxy\":["+dx+","+dy+","+(dx+w)+","+(dy+h)+"]}");
 }}
 g.Flush();
 using(var final=new Bitmap(512,512,PixelFormat.Format32bppArgb)){
 for(int y=0;y<512;y++)for(int x=0;x<512;x++){
 int at=128+(y*512+x)*4;
 if(y<384)result[at+3]=alpha.GetPixel(x,y).A;
 final.SetPixel(x,y,Color.FromArgb(result[at+3],result[at+2],result[at+1],result[at]));
 }
 final.Save(Path.Combine(output,"ope60_title.png"),ImageFormat.Png);
 using(var preview=new Bitmap(1024,1024))using(var pg=Graphics.FromImage(preview)){
 pg.Clear(Color.Black);pg.InterpolationMode=InterpolationMode.NearestNeighbor;pg.DrawImage(final,0,0,1024,1024);preview.Save(Path.Combine(output,"ope60_title-blackpreview.png"),ImageFormat.Png);
 }
 int[][] phrases={new int[]{0,1,2,3},new int[]{0,1,4,5},new int[]{6,7,4,5},new int[]{8,9,10,11},new int[]{6,7,12,13},new int[]{14,15}};
 using(var proof=new Bitmap(512,96*phrases.Length))using(var pg=Graphics.FromImage(proof)){
 pg.Clear(Color.Black);for(int row=0;row<phrases.Length;row++)for(int col=0;col<phrases[row].Length;col++){int index=phrases[row][col];pg.DrawImage(final,new Rectangle(col*128,row*96,128,96),new Rectangle((index%4)*128,(index/4)*96,128,96),GraphicsUnit.Pixel);}
 proof.Save(Path.Combine(output,"ope60_title-phrase-proof.png"),ImageFormat.Png);
 }
 }
 }
 // Exact structural preservation: header, every RGB byte, unused lower128 rows.
 for(int i=0;i<result.Length;i++)if(i<128||i>=128+512*384*4||(i-128)%4!=3)if(result[i]!=source[i])throw new Exception("Unauthorized changed byte "+i);
 File.WriteAllBytes(Path.Combine(output,"ope60_title.dds"),result);meta.Append("]");File.WriteAllText(Path.Combine(output,"gdi-glyph-crops.json"),meta.ToString());
 }
}
'@
[CautionGlyphAtlas]::Run($Glyphs,$Original,$Output)
