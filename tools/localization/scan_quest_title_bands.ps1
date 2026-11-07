$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;using System.Drawing;using System.Drawing.Imaging;using System.Collections.Generic;using System.Text;
public static class TitleBandScan {
public static string Run(string path,string output){using(var src=new Bitmap(path))using(var mask=new Bitmap(src.Width,src.Height,PixelFormat.Format32bppArgb)){
int[] counts=new int[src.Height];for(int y=0;y<src.Height;y++)for(int x=0;x<src.Width;x++){var c=src.GetPixel(x,y);int a=Math.Min(c.R,Math.Min(c.G,c.B))*c.A/255;if(a<64)a=0;mask.SetPixel(x,y,Color.FromArgb(a,255,255,255));if(a>=64)counts[y]++;}
var bands=new List<int[]>();int top=-1,last=-1;for(int y=0;y<src.Height;y++){if(counts[y]>=20){if(top<0)top=y;last=y;}else if(top>=0&&y-last>=3){bands.Add(new int[]{top,last+1});top=-1;}}if(top>=0)bands.Add(new int[]{top,last+1});
using(var black=new Bitmap(src.Width,src.Height))using(var g=Graphics.FromImage(black)){g.Clear(Color.Black);g.DrawImageUnscaled(mask,0,0);black.Save(output,ImageFormat.Png);}
var text=new StringBuilder("[");for(int n=0;n<bands.Count;n++){int left=src.Width,right=0;for(int y=bands[n][0];y<bands[n][1];y++)for(int x=0;x<src.Width;x++)if(mask.GetPixel(x,y).A>=64){left=Math.Min(left,x);right=Math.Max(right,x+1);}if(n>0)text.Append(",");int l=Math.Max(0,left-3),t=Math.Max(0,bands[n][0]-3),r=Math.Min(src.Width,right+3),b=Math.Min(src.Height,bands[n][1]+3);text.Append("["+l+","+t+","+(r-l)+","+(b-t)+"]");}return text.Append("]").ToString();}}
}
'@
$dir=Join-Path (Get-Location) 'localization/image_assets/caution-ui-v4/quest-title-glyphs'
foreach($n in 1..3){$result=[TitleBandScan]::Run((Join-Path $dir "sheet-$n.png"),(Join-Path $dir "sheet-$n-clean-black.png"));[System.IO.File]::WriteAllText((Join-Path $dir "sheet-$n-bands.json"),$result);Write-Output "sheet$n $result"}
