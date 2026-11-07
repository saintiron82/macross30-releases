param([Parameter(Mandatory=$true)][string]$GlyphMap,[string]$Plan='localization/image_assets/menu-ui-v7/misc-input/plan.json',[string]$Output='localization/image_assets/menu-ui-v7/misc-composited')
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing.Common,System.Drawing.Primitives,System.Private.Windows.GdiPlus,System.Private.Windows.Core,System.Collections -TypeDefinition @'
using System;using System.IO;using System.Drawing;using System.Drawing.Imaging;using System.Drawing.Drawing2D;using System.Collections.Generic;
public static class RegionNameGdi {
 public static int[][] Bands(Bitmap b,int expected){var result=new List<int[]>();int first=-1,last=-1;
  for(int y=0;y<b.Height;y++){int count=0;for(int x=0;x<b.Width;x++){var c=b.GetPixel(x,y);if(c.A>=128)count++;}
   if(count>=3){if(first<0)first=y;last=y;}else if(first>=0&&y-last>8){result.Add(new int[]{first,last});first=-1;}}
  if(first>=0)result.Add(new int[]{first,last});if(result.Count!=expected)throw new Exception("Glyph row count "+result.Count+" expected "+expected);return result.ToArray();}
 public static string Label(byte[] original,byte[] data,int w,int h,Bitmap sheet,int row,int count,int[] roi,string style){
  int[][] bands=Bands(sheet,count);int sy=bands[row][0],sh=bands[row][1]-sy+1;
  using(var mask=new Bitmap(sheet.Width,sh,PixelFormat.Format32bppArgb)){
   int left=sheet.Width,right=-1,top=sh,bottom=-1;
   for(int y=0;y<sh;y++)for(int x=0;x<sheet.Width;x++){var c=sheet.GetPixel(x,sy+y);int lum=Math.Min(c.R,Math.Min(c.G,c.B));int a=c.A<64?0:c.A;
    mask.SetPixel(x,y,Color.FromArgb(a,255,255,255));if(a>12){left=Math.Min(left,x);right=Math.Max(right,x);top=Math.Min(top,y);bottom=Math.Max(bottom,y);}}
   if(right<left)throw new Exception("No readable glyph");
   int x0=roi[0],y0=roi[1],rw=roi[2],rh=roi[3];int coreTop=h,coreBottom=-1;long[] sums=new long[3];int samples=0;
   bool cyanPanel=style=="cyan_panel",greenPanel=style=="cream_green_panel",black=style=="black_transparent";
   for(int y=y0;y<y0+rh;y++)for(int x=x0;x<x0+rw;x++){int at=(y*w+x)*4;bool isCore=original[at+3]>=160;
    if(cyanPanel||greenPanel)isCore=isCore&&original[at+2]>150&&original[at+1]>180&&original[at]>100;
    if(isCore){coreTop=Math.Min(coreTop,y);coreBottom=Math.Max(coreBottom,y);samples++;for(int c=0;c<3;c++)sums[c]+=original[at+2-c];}}
   int maxH=Math.Min(rh-2,coreBottom>=coreTop?coreBottom-coreTop+1:rh-4);if(style.StartsWith("cream_gold"))maxH=Math.Min(rh-4,maxH);
   var crop=new Rectangle(left,top,right-left+1,bottom-top+1);double scale=Math.Min((rw-4.0)/crop.Width,maxH/(double)crop.Height);
   int nw=Math.Max(1,(int)Math.Round(crop.Width*scale)),nh=Math.Max(1,(int)Math.Round(crop.Height*scale)),ox=x0+(rw-nw)/2,oy=y0+(rh-nh)/2;
   byte[] alpha=new byte[w*h];using(var small=new Bitmap(nw,nh,PixelFormat.Format32bppArgb)){using(var g=Graphics.FromImage(small)){g.CompositingMode=CompositingMode.SourceCopy;g.InterpolationMode=InterpolationMode.HighQualityBicubic;g.PixelOffsetMode=PixelOffsetMode.HighQuality;g.DrawImage(mask,new Rectangle(0,0,nw,nh),crop,GraphicsUnit.Pixel);}
    for(int y=0;y<nh;y++)for(int x=0;x<nw;x++){int a=small.GetPixel(x,y).A;alpha[(oy+y)*w+ox+x]=(byte)(a<48?0:Math.Round((a-48)*255/207.0));}}
   int[] tint=samples>0?new int[]{(int)(sums[0]/samples),(int)(sums[1]/samples),(int)(sums[2]/samples)}:new int[]{255,255,255};
   if(black)tint=new int[]{0,0,0};if(cyanPanel)tint=new int[]{230,255,255};if(greenPanel)tint=new int[]{248,252,190};
   int[] glowTint=style.StartsWith("cyan")?new int[]{0,210,210}:style.StartsWith("red")?new int[]{255,40,70}:style.StartsWith("green")?new int[]{100,210,0}:style.StartsWith("gold")||style.StartsWith("cream")?new int[]{255,190,0}:tint;
   for(int y=y0;y<y0+rh;y++){
    int[] panelBase=new int[4];if(cyanPanel){int hits=0;for(int x=x0+10;x<x0+rw-9;x++){int at=(y*w+x)*4;if(original[at+2]==0){hits++;for(int c=0;c<4;c++)panelBase[c]+=original[at+c];}}
     if(hits>0)for(int c=0;c<4;c++)panelBase[c]/=hits;else{panelBase[0]=255;panelBase[1]=210;panelBase[2]=0;panelBase[3]=110;}}
    for(int x=x0;x<x0+rw;x++){int pi=y*w+x,at=pi*4;
     if(cyanPanel){if(original[at+2]>0){for(int c=0;c<4;c++)data[at+c]=(byte)panelBase[c];}}
     else if(greenPanel){int a1=(y*w+x0-1)*4,a2=(y*w+x0+rw)*4;double t=(x-x0+1.0)/(rw+1);for(int c=0;c<4;c++)data[at+c]=(byte)Math.Round(original[a1+c]*(1-t)+original[a2+c]*t);}
     else data[at+3]=0;
     int a=alpha[pi],glow=0;if(!black)for(int dy=-2;dy<=2;dy++)for(int dx=-2;dx<=2;dx++){int xx=x+dx,yy=y+dy;double d=dx*dx+dy*dy;if(xx>=0&&xx<w&&yy>=0&&yy<h&&d>0&&d<=4)glow=Math.Max(glow,(int)(alpha[yy*w+xx]*0.25*Math.Exp(-d/1.5)));}
     double ga=glow/255.0,ca=a/255.0,fg=ca+ga*(1-ca),bg=data[at+3]/255.0*(1-fg),oa=fg+bg;
     if(fg>0&&oa>0){for(int c=0;c<3;c++){double color=(tint[c]*ca+glowTint[c]*ga*(1-ca))/fg;data[at+2-c]=(byte)Math.Clamp((int)Math.Round((color*fg+data[at+2-c]*bg)/oa),0,255);}data[at+3]=(byte)Math.Clamp((int)Math.Round(oa*255),0,255);}
    }
   }
   return "{\"crop_xywh\":["+left+","+(sy+top)+","+crop.Width+","+crop.Height+"],\"destination_xywh\":["+ox+","+oy+","+nw+","+nh+"],\"row\":"+row+",\"style\":\""+style+"\"}";
  }
 }
 public static void Output(byte[] data,int w,int h,string stem){using(var b=new Bitmap(w,h,PixelFormat.Format32bppArgb)){var rect=new Rectangle(0,0,w,h);var l=b.LockBits(rect,ImageLockMode.WriteOnly,PixelFormat.Format32bppArgb);System.Runtime.InteropServices.Marshal.Copy(data,0,l.Scan0,data.Length);b.UnlockBits(l);b.Save(stem+".png",ImageFormat.Png);
  foreach(bool white in new[]{false,true})using(var canvas=new Bitmap(w*3,h*3)){using(var g=Graphics.FromImage(canvas)){g.Clear(white?Color.White:Color.Black);g.InterpolationMode=InterpolationMode.NearestNeighbor;g.PixelOffsetMode=PixelOffsetMode.Half;g.DrawImage(b,new Rectangle(0,0,w*3,h*3));}canvas.Save(stem+(white?"-white3x.png":"-black3x.png"),ImageFormat.Png);}}
  using(var f=new BinaryWriter(File.Create(stem+".rgba")))for(int i=0;i<data.Length;i+=4){f.Write(data[i+2]);f.Write(data[i+1]);f.Write(data[i]);f.Write(data[i+3]);}
 }
}
'@
$meta=Get-Content $Plan -Raw -Encoding utf8|ConvertFrom-Json
$map=Get-Content $GlyphMap -Raw -Encoding utf8|ConvertFrom-Json
if(Test-Path $Output){throw 'Choose new output directory for each reviewed composition'}
New-Item -ItemType Directory -Force $Output|Out-Null
$reports=@()
foreach($asset in $meta.assets){
 $activeLabels=@($asset.labels|Where-Object { $gid=$_.glyph_id;@($map.rows|Where-Object glyph_id -eq $gid).Count -gt 0 });if(!$activeLabels.Count){continue}
 [byte[]]$source=[IO.File]::ReadAllBytes($asset.source_BGRA);[byte[]]$bytes=$source.Clone();$w=$asset.width;$h=$asset.height;$allowed=[bool[]]::new($w*$h);$proof=@()
 if(([Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($source)).ToLowerInvariant()) -ne $asset.source_BGRA_sha256){throw 'Source BGRA changed'}
 foreach($label in $activeLabels){$glyph=@($map.rows|Where-Object glyph_id -eq $label.glyph_id);if($glyph.Count -ne 1){throw 'Missing/duplicate glyph map'};$glyph=$glyph[0];$roi=[int[]]$label.roi_xywh
  for($y=$roi[1];$y -lt $roi[1]+$roi[3];$y++){for($x=$roi[0];$x -lt $roi[0]+$roi[2];$x++){if($allowed[$y*$w+$x]){throw 'Overlapping ROI'};$allowed[$y*$w+$x]=$true}}
  $sheet=[Drawing.Bitmap]::new((Resolve-Path $glyph.sheet).Path);try{$detail=[RegionNameGdi]::Label($source,$bytes,$w,$h,$sheet,$glyph.row,$glyph.row_count,$roi,$label.style)|ConvertFrom-Json}finally{$sheet.Dispose()}
  $proof+=@{glyph_id=$label.glyph_id;source=$label.source;target=$label.target;detail=$detail}
 }
 $outside=0;$changed=0;for($i=0;$i -lt $bytes.Length;$i++){if($bytes[$i] -ne $source[$i]){$changed++;if(!$allowed[[int][Math]::Floor($i/4)]){$outside++}}};if($outside){throw 'Outside ROI modified'}
 $stem=Join-Path $Output $asset.output_stem;[RegionNameGdi]::Output($bytes,$w,$h,$stem)
 $reports+=@{name=$asset.name;source_sha256=$asset.source_sha256;source_DDS=$asset.source_DDS;output_stem=$stem;format=$asset.format;width=$w;height=$h;labels=$proof;outside_ROI_bytes=$outside;changed_RGBA_bytes=$changed}
}
@{status='OFFLINE_GDI_COMPOSITION_NOT_INSTALLED';assets=$reports;glyph_map=$GlyphMap;plan=$Plan;active_modified=$false}|ConvertTo-Json -Depth 20|Set-Content (Join-Path $Output 'composition.json') -Encoding utf8
$reports|Select-Object name,format,changed_RGBA_bytes,outside_ROI_bytes|Format-Table
