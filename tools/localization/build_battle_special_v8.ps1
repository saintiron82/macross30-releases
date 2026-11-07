param([string]$Out='localization/image_assets/battle-hud-v8/candidate')
$ErrorActionPreference='Stop';Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing.Common,System.Drawing.Primitives,System.Private.Windows.GdiPlus,System.Private.Windows.Core,System.Collections -TypeDefinition @'
using System;using System.IO;using System.Collections.Generic;using System.Drawing;using System.Drawing.Imaging;using System.Drawing.Drawing2D;
public static class BattleSpecial {
 public static string Render(string input,string glyph,string output){
 byte[] source=File.ReadAllBytes(input),result=(byte[])source.Clone();int W=1280,H=720;var donors=new List<string>();
 using(var sheet=new Bitmap(glyph)){
 var bands=new List<int[]>();int first=-1,last=-1;
 for(int y=0;y<sheet.Height;y++){int n=0;for(int x=0;x<sheet.Width;x++)if(sheet.GetPixel(x,y).A>=128)n++;if(n>=3){if(first<0)first=y;last=y;}else if(first>=0&&y-last>8){bands.Add(new[]{first,last});first=-1;}}
 if(first>=0)bands.Add(new[]{first,last});if(bands.Count!=12)throw new Exception("Expected12 glyphrows "+bands.Count);
 int top=bands[7][0],bottom=bands[7][1],left=sheet.Width,right=-1;for(int y=top;y<=bottom;y++)for(int x=0;x<sheet.Width;x++)if(sheet.GetPixel(x,y).A>=128){left=Math.Min(left,x);right=Math.Max(right,x);}
 using(var mask=new Bitmap(right-left+1,bottom-top+1,PixelFormat.Format32bppArgb)){
 for(int y=0;y<mask.Height;y++)for(int x=0;x<mask.Width;x++){int a=sheet.GetPixel(left+x,top+y).A;mask.SetPixel(x,y,Color.FromArgb(a<64?0:a,0,0,0));}
 double scale=Math.Min(23.0/mask.Width,12.0/mask.Height);int dw=(int)Math.Round(mask.Width*scale),dh=(int)Math.Round(mask.Height*scale);
 using(var small=new Bitmap(dw,dh,PixelFormat.Format32bppArgb)){
 using(var g=Graphics.FromImage(small)){g.CompositingMode=CompositingMode.SourceCopy;g.InterpolationMode=InterpolationMode.HighQualityBicubic;g.PixelOffsetMode=PixelOffsetMode.HighQuality;g.DrawImage(mask,new Rectangle(0,0,dw,dh));}
 foreach(int y0 in new[]{358,377,396})for(int y=y0;y<y0+14;y++){
 int baseA=-1,hits=0;var xs=new List<int>();
 for(int x=858;x<=892;x++){int k=128+(y*W+x)*4;if(source[k]==0&&source[k+1]==255&&source[k+2]==190){int a=source[k+3];if(baseA<0)baseA=a;else if(baseA!=a)throw new Exception("Donor alpha mismatch");hits++;xs.Add(x);}}
 int donorY=y;
 if(hits<2){int observed=baseA;bool found=false;for(int sy=y0;sy<y0+13;sy++){if((sy-y)%3!=0)continue;var sx=new List<int>();int a0=-1;for(int x=858;x<=892;x++){int k=128+(sy*W+x)*4;if(source[k]==0&&source[k+1]==255&&source[k+2]==190){if(a0<0)a0=source[k+3];else if(a0!=source[k+3])throw new Exception("Pattern donor mismatch");sx.Add(x);}}if(sx.Count>=2){if(observed>=0&&observed!=a0)throw new Exception("Observed scanline alpha disagrees with periodic donor");baseA=a0;hits=sx.Count;xs=sx;donorY=sy;found=true;break;}}if(!found)throw new Exception("No verified 3-row panel-period donor");}
 donors.Add(y+":"+baseA+":donorY="+donorY+":x="+string.Join(",",xs));
 for(int x=858;x<=882;x++){int k=128+(y*W+x)*4;int dx=x-(858+(25-dw)/2),dy=y-(y0+(14-dh)/2);double f=(dx>=0&&dx<dw&&dy>=0&&dy<dh?small.GetPixel(dx,dy).A:0)/255.0*.75;double ba=baseA/255.0,oa=f+ba*(1-f);result[k]=0;result[k+1]=(byte)Math.Round(255*ba*(1-f)/oa);result[k+2]=(byte)Math.Round(190*ba*(1-f)/oa);result[k+3]=(byte)Math.Round(oa*255);}
 }
 }
 }
 }
 File.WriteAllBytes(output+".dds",result);
 foreach(bool after in new[]{false,true}){byte[] b=after?result:source;using(var im=new Bitmap(W,H,PixelFormat.Format32bppArgb)){var l=im.LockBits(new Rectangle(0,0,W,H),ImageLockMode.WriteOnly,PixelFormat.Format32bppArgb);System.Runtime.InteropServices.Marshal.Copy(b,128,l.Scan0,W*H*4);im.UnlockBits(l);im.Save(output+(after?".png":"-before.png"),ImageFormat.Png);using(var c=new Bitmap(480,354)){using(var g=Graphics.FromImage(c)){g.Clear(Color.White);g.InterpolationMode=InterpolationMode.NearestNeighbor;g.DrawImage(im,new Rectangle(0,0,480,354),new Rectangle(855,354,80,59),GraphicsUnit.Pixel);}c.Save(output+(after?"-special-after6x.png":"-special-before6x.png"),ImageFormat.Png);}}}
 return string.Join("\n",donors);
 }
}
'@
if(Test-Path $Out){throw 'Fresh output directory required'};New-Item -ItemType Directory -Force $Out|Out-Null
$stem=Join-Path $Out 'hud_cockpit_01';$proof=[BattleSpecial]::Render((Resolve-Path 'analysis/localization/battle-special-audit/disc-hud_cockpit_01.dds').Path,(Resolve-Path 'localization/image_assets/menu-ui-v7/generated/hangar-black-12.png').Path,(Join-Path (Get-Location) $stem));$proof|Set-Content (Join-Path $Out 'scanline-donors.txt')
