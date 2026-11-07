param([string]$Plan='analysis/localization/menu-v7-audit/gallery-title-13-glyph-plan.json',
 [string]$Glyph='localization/image_assets/menu-ui-v7/generated/gallery-title-13.png',
 [string]$Output='localization/image_assets/menu-ui-v7/gallery-title-rgba')
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;using System.IO;using System.Drawing;using System.Drawing.Imaging;using System.Drawing.Drawing2D;using System.Collections.Generic;
public static class GalleryTitleV7 {
 static Bitmap LoadClone(string path){using(var loaded=new Bitmap(path))return new Bitmap(loaded);}
 static bool WhiteGlyph(Color c){return c.A>=24&&Math.Min(c.R,Math.Min(c.G,c.B))>=225&&Math.Max(c.R,Math.Max(c.G,c.B))-Math.Min(c.R,Math.Min(c.G,c.B))<90;}
 static Color CapsuleBackground(Bitmap b,int x,int y,int top,int bottom){
  int a=y-1,z=y+1;while(a>=Math.Max(0,top-3)&&WhiteGlyph(b.GetPixel(x,a)))a--;
  while(z<Math.Min(b.Height,bottom+3)&&WhiteGlyph(b.GetPixel(x,z)))z++;
  bool aa=a>=Math.Max(0,top-3),zz=z<Math.Min(b.Height,bottom+3);
  if(!aa&&!zz)throw new Exception("No capsule background donor");
  if(!aa)return b.GetPixel(x,z);if(!zz)return b.GetPixel(x,a);
  Color u=b.GetPixel(x,a),v=b.GetPixel(x,z);double t=(double)(y-a)/(z-a);
  return Color.FromArgb((int)Math.Round(u.A*(1-t)+v.A*t),(int)Math.Round(u.R*(1-t)+v.R*t),(int)Math.Round(u.G*(1-t)+v.G*t),(int)Math.Round(u.B*(1-t)+v.B*t));
 }
 public static string[] Bands(string path){using(var b=new Bitmap(path)){
  var bands=new List<int[]>();int begin=-1,last=-1;
  for(int y=0;y<b.Height;y++){int n=0;for(int x=0;x<b.Width;x++)if(b.GetPixel(x,y).A>=128)n++;
   if(n>=10){if(begin<0)begin=y;if(last>=0&&y-last>18){bands.Add(new int[]{begin,last+1});begin=y;}last=y;}}
  if(begin>=0)bands.Add(new int[]{begin,last+1});if(bands.Count!=13)throw new Exception("Expected13 alpha bands, got"+bands.Count);
  var result=new List<string>();foreach(var band in bands){int left=b.Width,right=0;for(int y=band[0];y<band[1];y++)for(int x=0;x<b.Width;x++)if(b.GetPixel(x,y).A>=128){left=Math.Min(left,x);right=Math.Max(right,x+1);}result.Add(left+","+band[0]+","+(right-left)+","+(band[1]-band[0]));}return result.ToArray();
 }}
 public static string Render(string glyph,string basisPath,string output,int sx,int sy,int sw,int sh,int rx,int ry,int rw,int rh,bool capsule){
 using(var source=new Bitmap(glyph))using(var basis=LoadClone(basisPath))using(var original=LoadClone(basisPath))using(var mask=new Bitmap(sw,sh,PixelFormat.Format32bppArgb)){
  for(int y=0;y<sh;y++)for(int x=0;x<sw;x++){int a=source.GetPixel(sx+x,sy+y).A;if(a<64)a=0;mask.SetPixel(x,y,Color.FromArgb(a,255,255,255));}
  int margin=capsule?3:2;float scale=Math.Min((rw-margin*2f)/sw,(rh-2f)/sh);int dw=Math.Max(1,(int)Math.Round(sw*scale)),dh=Math.Max(1,(int)Math.Round(sh*scale));
  int dx=rx+(rw-dw)/2,dy=ry+(rh-dh)/2;
  using(var scaled=new Bitmap(rw,rh,PixelFormat.Format32bppArgb)){
   using(var g=Graphics.FromImage(scaled)){g.Clear(Color.Transparent);g.InterpolationMode=InterpolationMode.HighQualityBicubic;g.PixelOffsetMode=PixelOffsetMode.HighQuality;g.DrawImage(mask,new Rectangle(dx-rx,dy-ry,dw,dh),new Rectangle(0,0,sw,sh),GraphicsUnit.Pixel);}
   var rowColors=new Color[rh];
   for(int y=0;y<rh;y++){var counts=new Dictionary<int,int>();int best=0,bestN=0;for(int x=0;x<rw;x++){Color c=original.GetPixel(rx+x,ry+y);if(c.A<128)continue;int k=c.ToArgb()&0xFFFFFF;int n;counts.TryGetValue(k,out n);counts[k]=++n;if(n>bestN){bestN=n;best=k;}}rowColors[y]=bestN>0?Color.FromArgb(255,(best>>16)&255,(best>>8)&255,best&255):Color.White;}
   // Capsule bases are retained. Only the old white glyph coverage is
   // reconstructed from this exact capsule's adjacent original row donors.
   int reconstructed=0;byte[] touched=new byte[basis.Width*basis.Height];
   if(capsule)for(int y=ry;y<ry+rh;y++)for(int x=rx;x<rx+rw;x++)if(WhiteGlyph(original.GetPixel(x,y))){basis.SetPixel(x,y,CapsuleBackground(original,x,y,ry,ry+rh));touched[y*basis.Width+x]=1;reconstructed++;}
   for(int y=0;y<rh;y++)for(int x=0;x<rw;x++){
    int alpha=scaled.GetPixel(x,y).A;
    if(capsule){if(alpha==0)continue;touched[(ry+y)*basis.Width+rx+x]=1;Color bg=basis.GetPixel(rx+x,ry+y);float t=alpha/255f;basis.SetPixel(rx+x,ry+y,Color.FromArgb(Math.Max(bg.A,alpha),(int)Math.Round(bg.R*(1-t)+255*t),(int)Math.Round(bg.G*(1-t)+255*t),(int)Math.Round(bg.B*(1-t)+255*t)));}
    else{touched[(ry+y)*basis.Width+rx+x]=1;Color color=rowColors[y];basis.SetPixel(rx+x,ry+y,Color.FromArgb(alpha,color.R,color.G,color.B));}
   }
   Directory.CreateDirectory(Path.GetDirectoryName(output));basis.Save(output+".png",ImageFormat.Png);
   using(var f=new BinaryWriter(File.Create(output+".rgba")))for(int y=0;y<basis.Height;y++)for(int x=0;x<basis.Width;x++){Color c=basis.GetPixel(x,y);f.Write(c.R);f.Write(c.G);f.Write(c.B);f.Write(c.A);}
   File.WriteAllBytes(output+"."+rx+"-"+ry+".mask",touched);
   using(var preview=new Bitmap(basis.Width*4,basis.Height*4))using(var g=Graphics.FromImage(preview)){g.Clear(Color.Black);g.InterpolationMode=InterpolationMode.NearestNeighbor;g.DrawImage(basis,0,0,preview.Width,preview.Height);preview.Save(output+".black.png",ImageFormat.Png);}
   return "{\"source_cell_xywh\":["+sx+","+sy+","+sw+","+sh+"],\"rendered_size\":["+dw+","+dh+"],\"capsule_old_glyph_pixels_reconstructed\":"+reconstructed+",\"alpha_only\":true,\"alpha_cutoff\":64}";
  }
 }}
}
'@
$doc=Get-Content -LiteralPath $Plan -Raw -Encoding UTF8 | ConvertFrom-Json
$cells=[GalleryTitleV7]::Bands([IO.Path]::GetFullPath($Glyph))
New-Item -ItemType Directory -Path $Output -Force | Out-Null
$reports=@();$done=@{}
foreach($row in $doc.rows){
 if((Get-FileHash -LiteralPath $row.source_DDS -Algorithm SHA256).Hash.ToLowerInvariant() -ne $row.source_SHA256){throw 'Original DDS changed'}
 $stem=[IO.Path]::GetFileNameWithoutExtension($row.source_filename)
 $basis=if($done[$stem]){Join-Path $Output ($stem+'.png')}else{[IO.Path]::ChangeExtension($row.source_DDS,'png')}
 $c=@($cells[$row.order-1].Split(',') | ForEach-Object {[int]$_});$r=$row.UV_xywh
 $detail=[GalleryTitleV7]::Render([IO.Path]::GetFullPath($Glyph),[IO.Path]::GetFullPath($basis),[IO.Path]::GetFullPath((Join-Path $Output $stem)),$c[0],$c[1],$c[2],$c[3],$r[0],$r[1],$r[2],$r[3],[bool]$row.colored_capsule_background_present) | ConvertFrom-Json
 $done[$stem]=$true;$reports+=@{order=$row.order;JP=$row.JP;KO=$row.KO_draft;source_filename=$row.source_filename;UV_xywh=$r;detail=$detail}
}
@{compositor='GDI+';actual_alpha_bands=13;glyph=[IO.Path]::GetFullPath($Glyph);rows=$reports} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $Output 'composition.json') -Encoding UTF8
