param([switch]$Replace)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing.Common,System.Drawing.Primitives,System.Private.Windows.GdiPlus,System.Private.Windows.Core,System.Security.Cryptography,System.Collections -TypeDefinition @"
using System;
using System.IO;
using System.Drawing;
using System.Drawing.Imaging;
using System.Drawing.Drawing2D;
public class MenuLabelProof { public int row; public int[] source_crop,destination,edit_roi; public int original_nonzero_alpha,new_nonzero_alpha; public int[] average_core_rgb; }
public static class OtherBuilder {
 public static MenuLabelProof Label(byte[] original,byte[] data,Bitmap sheet,int row,int[] roi,int[] target,string mode,int sample) {
 int w=BitConverter.ToInt32(original,16),h=BitConverter.ToInt32(original,12);
 var bands=new System.Collections.Generic.List<int[]>();int first=-1,last=-1;
 for(int sy=0;sy<sheet.Height;sy++){int count=0;for(int sx=0;sx<sheet.Width;sx++){Color c=sheet.GetPixel(sx,sy);if(c.A>=160 && c.R>=180 && c.G>=180 && c.B>=180)count++;}
 if(count>=4){if(first<0)first=sy;last=sy;}else if(first>=0 && sy-last>7){bands.Add(new int[]{first,last});first=-1;}}
 if(first>=0)bands.Add(new int[]{first,last});if(bands.Count!=17)throw new Exception("Detected glyph rows "+bands.Count+" expected17");
 int sourceY=bands[row][0],band=bands[row][1]-sourceY+1;
 using(var mask=new Bitmap(sheet.Width,band,PixelFormat.Format32bppArgb)){
 int left=sheet.Width,top=band,right=-1,bottom=-1;
 for(int y=0;y<band;y++)for(int x=0;x<sheet.Width;x++){
 Color c=sheet.GetPixel(x,sourceY+y);int min=Math.Min(c.R,Math.Min(c.G,c.B));
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
 int at=128+4*(y*w+x);if(original[at+3]<160)continue;if(mode=="black" && (original[at+1]<180 || original[at+2]<100))continue;
 for(int c=0;c<3;c++)total[c]+=original[at+2-c];weight++;}
 if(weight==0){total[0]=220;total[1]=202;total[2]=80;weight=1;}
 int[] fallback=mode=="black"?new int[]{38,48,0}:new int[]{(int)(total[0]/weight),(int)(total[1]/weight),(int)(total[2]/weight)};
 byte[] alpha=new byte[w*h];for(int y=0;y<nh;y++)for(int x=0;x<nw;x++){int coverage=small.GetPixel(x,y).A;alpha[(oy+y)*w+ox+x]=(byte)(coverage<64?0:Math.Round((coverage-64)*255/191.0));}
 int oldcount=0,newcount=0;
 for(int y=roi[1];y<roi[1]+roi[3];y++){
 double[] rt=new double[3];double rw=0;
 for(int x=target[0];x<target[0]+target[2];x++){int at=128+4*(y*w+x);if(original[at+3]>=160 && (mode!="black" || original[at+1]>180)){rw++;for(int c=0;c<3;c++)rt[c]+=original[at+2-c];}}
 int[] tint=mode=="gray"?new int[]{244,218,104}:mode=="green"?new int[]{244,249,178}:mode=="black"?fallback:rw>0?new int[]{(int)(rt[0]/rw),(int)(rt[1]/rw),(int)(rt[2]/rw)}:fallback;
 for(int x=roi[0];x<roi[0]+roi[2];x++){
 int pi=y*w+x,at=128+pi*4;if(original[at+3]>0)oldcount++;
 if(mode=="black"){
 int lx=x%58,ly=y%30,maxB=0;
 for(int cy=0;cy<2;cy++)for(int cx=0;cx<4;cx++){int q=128+4*((cy*30+ly)*w+cx*58+lx);maxB=Math.Max(maxB,original[q]);}
 int reference=128+4*(ly*w+10);int green=original[reference+1];data[at]=(byte)maxB;data[at+1]=(byte)green;data[at+2]=(byte)Math.Clamp(green-65+maxB,0,255);data[at+3]=original[reference+3];
 }else if(mode=="gray"){if(original[at+2]>original[at]+8 || original[at+1]>original[at]+8){data[at]=46;data[at+1]=46;data[at+2]=46;data[at+3]=212;}}
 else if(mode=="green"){
 int q1=128+4*(y*w+roi[0]-1),q2=128+4*(y*w+roi[0]+roi[2]);double t=(x-roi[0]+1.0)/(roi[2]+1.0);
 for(int c=0;c<4;c++)data[at+c]=(byte)Math.Round(original[q1+c]*(1-t)+original[q2+c]*t);
 }else data[at+3]=0;
 int a=alpha[pi],glow=0;
 for(int dy=-2;dy<=2;dy++)for(int dx=-2;dx<=2;dx++){int xx=x+dx,yy=y+dy;if(xx<0||xx>=w||yy<0||yy>=h)continue;double dist=Math.Sqrt(dx*dx+dy*dy);if(dist>0 && dist<=2)glow=Math.Max(glow,(int)(alpha[yy*w+xx]*0.22*Math.Exp(-dist*dist/1.5)));}
 double fg=a/255.0+(mode=="black"?0:glow/255.0)*(1-a/255.0),bg=data[at+3]/255.0*(1-fg),oa=fg+bg;
 if(fg>0 && oa>0){for(int c=0;c<3;c++)data[at+2-c]=(byte)Math.Clamp((int)Math.Round((tint[c]*fg+data[at+2-c]*bg)/oa),0,255);data[at+3]=(byte)Math.Clamp((int)Math.Round(oa*255),0,255);}
 if(data[at+3]>0)newcount++;
 }}
 if(newcount==0)throw new Exception("Empty translated ROI");
 return new MenuLabelProof{row=row,source_crop=new int[]{left,top+sourceY,crop.Width,crop.Height},destination=new int[]{ox,oy,nw,nh},edit_roi=roi,original_nonzero_alpha=oldcount,new_nonzero_alpha=newcount,average_core_rgb=fallback};
 }}
 }
 public static void Preview(byte[] data,string path,bool black,int factor){int w=BitConverter.ToInt32(data,16),h=BitConverter.ToInt32(data,12);using(var im=new Bitmap(w,h,PixelFormat.Format32bppArgb)){for(int y=0;y<h;y++)for(int x=0;x<w;x++){int at=128+4*(y*w+x);im.SetPixel(x,y,Color.FromArgb(data[at+3],data[at+2],data[at+1],data[at]));}using(var output=new Bitmap(w*factor,h*factor,PixelFormat.Format32bppArgb)){using(var g=Graphics.FromImage(output)){g.Clear(black?Color.Black:Color.Transparent);g.InterpolationMode=InterpolationMode.NearestNeighbor;g.PixelOffsetMode=PixelOffsetMode.Half;g.DrawImage(im,new Rectangle(0,0,w*factor,h*factor));}output.Save(path,ImageFormat.Png);}}}
}
"@
function Sha([byte[]]$bytes){[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()}
$base='localization/image_assets/data-ui-v6';$meta=Get-Content "$base/other-final-rois.json" -Raw -Encoding utf8|ConvertFrom-Json
$sheet=[Drawing.Bitmap]::new((Resolve-Path "$base/generated/hangar-other-17.png").Path);$all=@()
try{foreach($group in ($meta.labels|Group-Object asset)){
 $labels=$group.Group;[byte[]]$source=[IO.File]::ReadAllBytes($labels[0].source);[byte[]]$data=$source.Clone()
 if((Sha $source) -ne $labels[0].source_sha256){throw 'Source SHA mismatch'}
 $w=[BitConverter]::ToInt32($source,16);$h=[BitConverter]::ToInt32($source,12);$allow=[bool[]]::new($w*$h);$proof=@()
 foreach($label in $labels){$roi=[int[]]$label.edit_roi;foreach($y in $roi[1]..($roi[1]+$roi[3]-1)){foreach($x in $roi[0]..($roi[0]+$roi[2]-1)){$pi=$y*$w+$x;if($allow[$pi]){throw 'Overlapping ROI'};$allow[$pi]=$true}};$proof += [OtherBuilder]::Label($source,$data,$sheet,$label.row,$roi,[int[]]$label.target,$label.mode,$label.sample)}
 $outside=0;$changed=0;for($i=0;$i -lt $data.Length;$i++){if($source[$i] -ne $data[$i]){$changed++;if($i -lt 128 -or !$allow[[int][Math]::Floor(($i-128)/4)]){$outside++}}};if($outside){throw 'Protected byte changed'}
 $out=Join-Path "$base/replacements" $group.Name;if((Test-Path $out) -and !$Replace){throw 'Output already exists'}
 [IO.File]::WriteAllBytes($out,$data);$stem=[IO.Path]::GetFileNameWithoutExtension($group.Name)
 [OtherBuilder]::Preview($data,"$base/replacements/$stem.png",$false,1);[OtherBuilder]::Preview($data,"$base/replacements/$stem-black-4x.png",$true,4)
 $all+=@{asset=$group.Name;source=$labels[0].source;source_sha256=(Sha $source);output_sha256=(Sha $data);labels=$labels.Count;changed_bytes=$changed;outside_roi_changed_bytes=$outside;header_changed_bytes=0;glyph_proofs=$proof}
}}
finally{$sheet.Dispose()}
@{status='candidate_not_installed';glyph_sheet="$base/generated/hangar-other-17.png";assets=$all;active_modified=$false}|ConvertTo-Json -Depth 12|Set-Content "$base/replacements/hangar-other-manifest.json" -Encoding utf8
$all|Select-Object asset,labels,changed_bytes,output_sha256|Format-Table -AutoSize

