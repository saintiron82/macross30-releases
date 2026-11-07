param(
 [string]$GlyphSheet='localization/image_assets/data-ui-v6/generated/hangar-menu01-24.png',
 [string]$RoiManifest='native/analysis/develop-ui-localization/menu01-final-rois.json',
 [string]$OutputDirectory='localization/image_assets/data-ui-v6/replacements',
 [switch]$Replace
)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing.Common,System.Drawing.Primitives,System.Private.Windows.GdiPlus,System.Private.Windows.Core,System.Security.Cryptography -TypeDefinition @"
using System;
using System.IO;
using System.Drawing;
using System.Drawing.Imaging;
using System.Drawing.Drawing2D;
public class MenuLabelProof { public int row; public int[] source_crop,destination,edit_roi; public int original_nonzero_alpha,new_nonzero_alpha; public int[] average_core_rgb; }
public static class Menu01Builder {
 public static MenuLabelProof Label(byte[] original,byte[] data,Bitmap sheet,int row,int[] roi,int[] target,bool panel) {
 int w=BitConverter.ToInt32(original,16),band=sheet.Height/24;
 if(sheet.Height%24!=0)throw new Exception("Expected exactly24 equal source row bands");
 using(var mask=new Bitmap(sheet.Width,band,PixelFormat.Format32bppArgb)){
 int left=sheet.Width,top=band,right=-1,bottom=-1;
 for(int y=0;y<band;y++)for(int x=0;x<sheet.Width;x++){
 Color c=sheet.GetPixel(x,row*band+y);int min=Math.Min(c.R,Math.Min(c.G,c.B));
 int a=c.A<160?0:(int)Math.Round((c.A-160)*255/95.0*Math.Clamp((min-180)/65.0,0,1));if(a<12)a=0;
 mask.SetPixel(x,y,Color.FromArgb(a,255,255,255));if(a>12){left=Math.Min(left,x);right=Math.Max(right,x);top=Math.Min(top,y);bottom=Math.Max(bottom,y);}}
 if(right<left)throw new Exception("Source row has no bright alpha glyph");
 var crop=new Rectangle(left,top,right-left+1,bottom-top+1);
 double scale=Math.Min(target[2]/(double)crop.Width,target[3]/(double)crop.Height);
 int nw=Math.Max(1,(int)Math.Round(crop.Width*scale)),nh=Math.Max(1,(int)Math.Round(crop.Height*scale)),ox=target[0]+(target[2]-nw)/2,oy=target[1]+(target[3]-nh)/2;
 using(var small=new Bitmap(nw,nh,PixelFormat.Format32bppArgb)){
 using(var g=Graphics.FromImage(small)){g.CompositingMode=CompositingMode.SourceCopy;g.InterpolationMode=InterpolationMode.HighQualityBicubic;g.PixelOffsetMode=PixelOffsetMode.HighQuality;g.DrawImage(mask,new Rectangle(0,0,nw,nh),crop,GraphicsUnit.Pixel);}
 double[] total=new double[3];double weight=0;
 for(int y=target[1];y<target[1]+target[3];y++)for(int x=target[0];x<target[0]+target[2];x++){
 int at=128+4*(y*w+x);if(original[at+3]<160)continue;if(panel && (original[at+1]<180 || original[at+2]<100))continue;
 for(int c=0;c<3;c++)total[c]+=original[at+2-c];weight++;}
 if(weight==0)throw new Exception("No original core palette");
 int[] fallback={(int)(total[0]/weight),(int)(total[1]/weight),(int)(total[2]/weight)};
 byte[] alpha=new byte[w*256];for(int y=0;y<nh;y++)for(int x=0;x<nw;x++){int coverage=small.GetPixel(x,y).A;alpha[(oy+y)*w+ox+x]=(byte)(coverage<64?0:Math.Round((coverage-64)*255/191.0));}
 int oldcount=0,newcount=0;
 for(int y=roi[1];y<roi[1]+roi[3];y++){
 double[] rt=new double[3];double rw=0;
 for(int x=target[0];x<target[0]+target[2];x++){int at=128+4*(y*w+x);if(original[at+3]>=160 && (!panel || original[at+1]>180)){rw++;for(int c=0;c<3;c++)rt[c]+=original[at+2-c];}}
 int[] tint=rw>0?new int[]{(int)(rt[0]/rw),(int)(rt[1]/rw),(int)(rt[2]/rw)}:fallback;
 for(int x=roi[0];x<roi[0]+roi[2];x++){
 int pi=y*w+x,at=128+pi*4;if(original[at+3]>0)oldcount++;
 if(panel){if(original[at+1]>original[at+2]+12){int sample=128+4*(y*w+300);for(int c=0;c<4;c++)data[at+c]=original[sample+c];}}
 else data[at+3]=0;
 int a=alpha[pi],glow=0;
 for(int dy=-2;dy<=2;dy++)for(int dx=-2;dx<=2;dx++){int xx=x+dx,yy=y+dy;if(xx<0||xx>=w||yy<0||yy>=256)continue;double dist=Math.Sqrt(dx*dx+dy*dy);if(dist>0 && dist<=2)glow=Math.Max(glow,(int)(alpha[yy*w+xx]*0.22*Math.Exp(-dist*dist/1.5)));}
 double fg=a/255.0+glow/255.0*(1-a/255.0),bg=data[at+3]/255.0*(1-fg),oa=fg+bg;
 if(fg>0 && oa>0){for(int c=0;c<3;c++)data[at+2-c]=(byte)Math.Clamp((int)Math.Round((tint[c]*fg+data[at+2-c]*bg)/oa),0,255);data[at+3]=(byte)Math.Clamp((int)Math.Round(oa*255),0,255);}
 if(data[at+3]>0)newcount++;
 }}
 if(newcount==0)throw new Exception("Empty translated ROI");
 return new MenuLabelProof{row=row,source_crop=new int[]{left,top+row*band,crop.Width,crop.Height},destination=new int[]{ox,oy,nw,nh},edit_roi=roi,original_nonzero_alpha=oldcount,new_nonzero_alpha=newcount,average_core_rgb=fallback};
 }}
 }
 public static void Preview(byte[] data,string path,bool black,int factor){int w=BitConverter.ToInt32(data,16),h=BitConverter.ToInt32(data,12);using(var im=new Bitmap(w,h,PixelFormat.Format32bppArgb)){for(int y=0;y<h;y++)for(int x=0;x<w;x++){int at=128+4*(y*w+x);im.SetPixel(x,y,Color.FromArgb(data[at+3],data[at+2],data[at+1],data[at]));}using(var output=new Bitmap(w*factor,h*factor,PixelFormat.Format32bppArgb)){using(var g=Graphics.FromImage(output)){g.Clear(black?Color.Black:Color.Transparent);g.InterpolationMode=InterpolationMode.NearestNeighbor;g.PixelOffsetMode=PixelOffsetMode.Half;g.DrawImage(im,new Rectangle(0,0,w*factor,h*factor));}output.Save(path,ImageFormat.Png);}}}
}
"@
function Sha([byte[]]$bytes){[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()}
$meta=Get-Content -LiteralPath $RoiManifest -Raw -Encoding utf8|ConvertFrom-Json
[byte[]]$source=[IO.File]::ReadAllBytes($meta.source);[byte[]]$data=$source.Clone()
if((Sha $source) -ne $meta.labels[0].source_sha256){throw 'Original source SHA mismatch'}
if($meta.labels.Count -ne 24 -or [BitConverter]::ToInt32($source,16) -ne 512 -or [BitConverter]::ToInt32($source,12) -ne 256){throw 'Wrong label set or DDS dimensions'}
$allow=[bool[]]::new(512*256);$proof=@();$sheet=[Drawing.Bitmap]::new((Resolve-Path $GlyphSheet).Path)
try{foreach($label in $meta.labels){$roi=[int[]]$label.edit_roi_xywh;foreach($y in $roi[1]..($roi[1]+$roi[3]-1)){foreach($x in $roi[0]..($roi[0]+$roi[2]-1)){$pi=$y*512+$x;if($allow[$pi]){throw 'Overlapping label edit regions'};$allow[$pi]=$true}};$proof += [Menu01Builder]::Label($source,$data,$sheet,$label.row,$roi,[int[]]$label.target_core_xywh,($label.row -eq 10))}}finally{$sheet.Dispose()}
$outside=0;$changed=0
for($i=0;$i -lt $data.Length;$i++){if($source[$i] -ne $data[$i]){$changed++;if($i -lt 128 -or !$allow[[int][Math]::Floor(($i-128)/4)]){$outside++}}}
if($outside){throw 'Header or protected byte changed'}
New-Item -ItemType Directory -Force $OutputDirectory|Out-Null
$dds=Join-Path $OutputDirectory 'hangar_menu01.dds';if((Test-Path $dds) -and !$Replace){throw 'Refuse overwrite of existing candidate without -Replace'}
[IO.File]::WriteAllBytes($dds,$data)
[Menu01Builder]::Preview($data,(Join-Path $OutputDirectory 'hangar_menu01.png'),$false,1)
[Menu01Builder]::Preview($data,(Join-Path $OutputDirectory 'hangar_menu01-black-4x.png'),$true,4)
@{status='candidate_not_installed';source=$meta.source;source_sha256=(Sha $source);output_sha256=(Sha $data);glyph_sheet=$GlyphSheet;glyph_sheet_sha256=(Sha ([IO.File]::ReadAllBytes($GlyphSheet)));roi_manifest=$RoiManifest;roi_manifest_sha256=(Sha ([IO.File]::ReadAllBytes($RoiManifest)));labels=24;changed_bytes=$changed;outside_roi_changed_bytes=$outside;header_changed_bytes=0;glyph_proofs=$proof;method='GDI alpha*bright-core mask with low-alpha haze rejected; aspect-preserving coverage resampling; original per-row RGB palette; bounded glow; green return label restores original same-row panel RGB/A';active_modified=$false}|ConvertTo-Json -Depth 10|Set-Content (Join-Path $OutputDirectory 'hangar-menu01-manifest.json') -Encoding utf8
Write-Output "24-label DDS built; changed=$changed outside=$outside SHA=$(Sha $data)"
