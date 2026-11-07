param([string]$Plan='analysis/localization/region-name-audit/b-c-f-s-definitive.json',
 [string]$Glyph='localization/image_assets/menu-ui-v7/generated/place-bcf-21.png',
 [string]$Output='localization/image_assets/menu-ui-v7/place-bcf-rgba')
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;using System.IO;using System.Drawing;using System.Drawing.Imaging;using System.Drawing.Drawing2D;using System.Collections.Generic;
public static class PlaceBcfV7 {
 public static string[] Bands(string path){using(var b=new Bitmap(path)){var bands=new List<int[]>();int start=-1,last=-1;
  for(int y=0;y<b.Height;y++){int n=0;for(int x=0;x<b.Width;x++)if(b.GetPixel(x,y).A>=128)n++;if(n>=10){if(start<0)start=y;if(last>=0&&y-last>8){bands.Add(new int[]{start,last+1});start=y;}last=y;}}
  if(start>=0)bands.Add(new int[]{start,last+1});if(bands.Count!=21)throw new Exception("Expected21 actual alpha bands, got"+bands.Count);
  var result=new List<string>();foreach(var r in bands){int left=b.Width,right=0;for(int y=r[0];y<r[1];y++)for(int x=0;x<b.Width;x++)if(b.GetPixel(x,y).A>=128){left=Math.Min(left,x);right=Math.Max(right,x+1);}result.Add(left+","+r[0]+","+(right-left)+","+(r[1]-r[0]));}return result.ToArray();}}
 static int Blur(byte[] a,int w,int h,int x,int y,int radius){int sum=0,n=0;for(int yy=Math.Max(0,y-radius);yy<=Math.Min(h-1,y+radius);yy++)for(int xx=Math.Max(0,x-radius);xx<=Math.Min(w-1,x+radius);xx++){sum+=a[yy*w+xx];n++;}return sum/n;}
 static int Dilate(byte[] a,int w,int h,int x,int y){int v=0;for(int yy=Math.Max(0,y-1);yy<=Math.Min(h-1,y+1);yy++)for(int xx=Math.Max(0,x-1);xx<=Math.Min(w-1,x+1);xx++)v=Math.Max(v,a[yy*w+xx]);return v;}
 public static string Render(string sourcePath,string glyphPath,string output,int sx,int sy,int sw,int sh,int bx,int by,int bw,int bh,bool center,bool glow,bool field){
 using(var original=new Bitmap(sourcePath))using(var glyph=new Bitmap(glyphPath))using(var mask=new Bitmap(sw,sh,PixelFormat.Format32bppArgb))using(var scaled=new Bitmap(original.Width,original.Height,PixelFormat.Format32bppArgb))using(var result=new Bitmap(original.Width,original.Height,PixelFormat.Format32bppArgb)){
  int w=original.Width,h=original.Height;float scale=Math.Min((bw-8f)/sw,(bh-8f)/sh);int dw=Math.Max(1,(int)Math.Round(sw*scale)),dh=Math.Max(1,(int)Math.Round(sh*scale));int dx=center?bx+(bw-dw)/2:bx+bw-dw-3,dy=by+(bh-dh)/2;
  if(dx<0||dy<0||dx+dw>w||dy+dh>h)throw new Exception("Glyph placement escapes original layer");
  for(int y=0;y<sh;y++)for(int x=0;x<sw;x++){int a=glyph.GetPixel(sx+x,sy+y).A;if(a<64)a=0;mask.SetPixel(x,y,Color.FromArgb(a,255,255,255));}
  using(var g=Graphics.FromImage(scaled)){g.Clear(Color.Transparent);g.InterpolationMode=InterpolationMode.HighQualityBicubic;g.PixelOffsetMode=PixelOffsetMode.HighQuality;g.DrawImage(mask,new Rectangle(dx,dy,dw,dh),new Rectangle(0,0,sw,sh),GraphicsUnit.Pixel);}
  byte[] core=new byte[w*h];for(int y=0;y<h;y++)for(int x=0;x<w;x++)core[y*w+x]=scaled.GetPixel(x,y).A;
  int maxAlpha=0;for(int y=0;y<h;y++)for(int x=0;x<w;x++)maxAlpha=Math.Max(maxAlpha,original.GetPixel(x,y).A);
  var all=new Dictionary<int,int>();int style=0,styleCount=0;
  for(int y=0;y<h;y++)for(int x=0;x<w;x++){Color c=original.GetPixel(x,y);if(c.A<maxAlpha/2||Math.Max(c.R,Math.Max(c.G,c.B))<96)continue;int k=c.ToArgb(),n;all.TryGetValue(k,out n);all[k]=++n;if(n>styleCount){style=k;styleCount=n;}}
  if(styleCount==0)throw new Exception("No original text palette");Color dominant=Color.FromArgb(style);
  Color[] rowColors=new Color[h];for(int yy=0;yy<h;yy++){var row=new Dictionary<int,int>();int best=0,bestN=0;for(int xx=0;xx<w;xx++){Color o=original.GetPixel(xx,yy);if(o.A<maxAlpha/2||Math.Max(o.R,Math.Max(o.G,o.B))<96)continue;int key=o.ToArgb(),n;row.TryGetValue(key,out n);row[key]=++n;if(n>bestN){best=key;bestN=n;}}rowColors[yy]=bestN>0?Color.FromArgb(best):dominant;}
  for(int y=0;y<h;y++)for(int x=0;x<w;x++){
   int a=core[y*w+x],blur=Blur(core,w,h,x,y,glow?3:2);Color c;
   if(glow){a=Math.Max(a*2/3,blur);c=Color.FromArgb(a*dominant.A/255,dominant.R,dominant.G,dominant.B);}
   else if(field){int final=Math.Max(a,blur*2/3);c=Color.FromArgb(final*dominant.A/255,dominant.R,dominant.G,dominant.B);}
   else {int edge=Dilate(core,w,h,x,y),alpha=Math.Max(a,edge);float t=alpha==0?0:a/(float)alpha;
    // Original silver/white face and dark outline; horizontal lighting is
    // sampled from original high-alpha visible pixels of the same row.
    int r=dominant.R,g=dominant.G,b=dominant.B;
    Color v=rowColors[y];r=v.R;g=v.G;b=v.B;
    c=Color.FromArgb(alpha*dominant.A/255,(int)(r*t),(int)(g*t),(int)(b*t));}
   result.SetPixel(x,y,c);
  }
  Directory.CreateDirectory(Path.GetDirectoryName(output));result.Save(output+".png",ImageFormat.Png);
  using(var f=new BinaryWriter(File.Create(output+".rgba")))for(int y=0;y<h;y++)for(int x=0;x<w;x++){Color c=result.GetPixel(x,y);f.Write(c.R);f.Write(c.G);f.Write(c.B);f.Write(c.A);}
  using(var preview=new Bitmap(w*2,h*2))using(var g=Graphics.FromImage(preview)){g.Clear(Color.Black);g.InterpolationMode=InterpolationMode.NearestNeighbor;g.DrawImage(result,0,0,w*2,h*2);preview.Save(output+".black.png",ImageFormat.Png);}
  return "{\"source_cell_xywh\":["+sx+","+sy+","+sw+","+sh+"],\"destination_xywh\":["+dx+","+dy+","+dw+","+dh+"],\"source_palette_ARGB\":["+dominant.A+","+dominant.R+","+dominant.G+","+dominant.B+"],\"original_alpha_max\":"+maxAlpha+",\"alpha_only_glyph\":true}";
 }}
}
'@
$doc=Get-Content -LiteralPath $Plan -Raw -Encoding UTF8 | ConvertFrom-Json
$cells=[PlaceBcfV7]::Bands([IO.Path]::GetFullPath($Glyph));New-Item -ItemType Directory -Path $Output -Force | Out-Null
$reports=@()
foreach($row in $doc.rows){
 $cell=@($cells[$row.order-1].Split(',') | ForEach-Object {[int]$_})
 $main=$row.layers_to_replace[0];$mb=$main.glyph_alpha_bbox_xyxy
 foreach($layer in $row.layers_to_replace){
  if((Get-FileHash -LiteralPath $layer.source_DDS -Algorithm SHA256).Hash.ToLowerInvariant() -ne $layer.source_SHA256){throw 'Original region DDS SHA changed'}
  $bb=if($layer.filename -eq 'placename_f00_title01.dds'){$layer.glyph_alpha_bbox_xyxy}else{$mb}
  $glow=$layer.layer_role -eq 'JP_glow_silhouette';$field=$row.family.StartsWith('placename_f');$center=$row.family.StartsWith('placename_c')
  $stem=[IO.Path]::GetFileNameWithoutExtension($layer.filename)
  $detail=[PlaceBcfV7]::Render([IO.Path]::GetFullPath($layer.source_PNG),[IO.Path]::GetFullPath($Glyph),[IO.Path]::GetFullPath((Join-Path $Output $stem)),$cell[0],$cell[1],$cell[2],$cell[3],$bb[0],$bb[1],($bb[2]-$bb[0]),($bb[3]-$bb[1]),$center,$glow,$field) | ConvertFrom-Json
  $reports+=@{order=$row.order;JP=$row.JP;KO=$row.KO;filename=$layer.filename;detail=$detail}
 }
}
@{status='GDI_COMPOSED_OFFLINE';rows=$reports;alpha_bands=21;source_alpha_cutoff=64} | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $Output 'composition.json') -Encoding UTF8
