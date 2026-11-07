param(
    [Parameter(Mandatory=$true)][string]$Audit,
    [Parameter(Mandatory=$true)][string]$Font,
    [Parameter(Mandatory=$true)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.IO;
using System.Drawing;
using System.Drawing.Text;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public class KoreanGlyphMetric {
    public int codepoint, width, height, bearing_x, bearing_y, advance;
}
public static class KoreanGlyphRasterizer {
    [DllImport("gdi32.dll", CharSet=CharSet.Unicode)] static extern int AddFontResourceEx(string path, uint flags, IntPtr reserved);
    [DllImport("gdi32.dll", CharSet=CharSet.Unicode)] static extern bool RemoveFontResourceEx(string path, uint flags, IntPtr reserved);
    [DllImport("gdi32.dll")] static extern IntPtr CreateCompatibleDC(IntPtr dc);
    [DllImport("gdi32.dll")] static extern IntPtr SelectObject(IntPtr dc, IntPtr obj);
    [DllImport("gdi32.dll")] static extern bool DeleteDC(IntPtr dc);
    [DllImport("gdi32.dll")] static extern bool DeleteObject(IntPtr obj);
    [DllImport("gdi32.dll", CharSet=CharSet.Unicode)] static extern uint GetGlyphIndices(IntPtr dc, string text, int count, ushort[] indices, uint flags);
    public static KoreanGlyphMetric[] Render(string path, int[] codes, string output) {
        const int cell=24, origin=8, canvas=64;
        if (AddFontResourceEx(path,16,IntPtr.Zero)==0) throw new Exception("Cannot privately register supplied font");
        try {
            using (PrivateFontCollection collection=new PrivateFontCollection()) {
                collection.AddFontFile(path);
                using (Font font=new Font(collection.Families[0],20f,FontStyle.Regular,GraphicsUnit.Pixel))
                using (Bitmap sheet=new Bitmap(32*28,((codes.Length+31)/32)*28,PixelFormat.Format32bppArgb))
                using (Graphics sg=Graphics.FromImage(sheet))
                using (BinaryWriter writer=new BinaryWriter(File.Create(Path.Combine(output,"glyph_alpha.bin")))) {
                    sg.Clear(Color.FromArgb(255,32,32,32));
                    KoreanGlyphMetric[] result=new KoreanGlyphMetric[codes.Length];
                    IntPtr hfont=font.ToHfont(), dc=CreateCompatibleDC(IntPtr.Zero), old=SelectObject(dc,hfont);
                    try {
                        for (int n=0;n<codes.Length;n++) {
                            string text=Char.ConvertFromUtf32(codes[n]); ushort[] indices=new ushort[1];
                            if(GetGlyphIndices(dc,text,1,indices,1)==0xffffffff || indices[0]==0xffff || indices[0]==0)
                                throw new Exception("Supplied font lacks glyph U+"+codes[n].ToString("X4"));
                            using(Bitmap bitmap=new Bitmap(canvas,canvas,PixelFormat.Format32bppArgb))
                            using(Graphics graphics=Graphics.FromImage(bitmap))
                            using(StringFormat format=(StringFormat)StringFormat.GenericTypographic.Clone()) {
                                graphics.Clear(Color.Transparent);
                                graphics.TextRenderingHint=TextRenderingHint.AntiAliasGridFit;
                                format.FormatFlags|=StringFormatFlags.NoClip|StringFormatFlags.NoFontFallback;
                                graphics.DrawString(text,font,Brushes.White,new PointF(origin,origin),format);
                                int left=canvas,top=canvas,right=-1,bottom=-1;
                                for(int y=0;y<canvas;y++) for(int x=0;x<canvas;x++) if(bitmap.GetPixel(x,y).A!=0) {
                                    left=Math.Min(left,x);top=Math.Min(top,y);right=Math.Max(right,x);bottom=Math.Max(bottom,y);
                                }
                                int width=right-left+1,height=bottom-top+1;
                                if(width<=0||height<=0||width>cell||height>cell) throw new Exception("Invalid raster bounds U+"+codes[n].ToString("X4"));
                                // Match existing Japanese full-width advance 19 and baseline metrics.
                                result[n]=new KoreanGlyphMetric {codepoint=codes[n],width=width,height=height,bearing_x=1,bearing_y=4,advance=19};
                                for(int y=0;y<cell;y++) for(int x=0;x<cell;x++)
                                    writer.Write((byte)(x<width&&y<height?bitmap.GetPixel(left+x,top+y).A:0));
                                sg.DrawImage(bitmap,new Rectangle((n%32)*28+2,(n/32)*28+2,width,height),new Rectangle(left,top,width,height),GraphicsUnit.Pixel);
                            }
                        }
                    } finally {SelectObject(dc,old);DeleteObject(hfont);DeleteDC(dc);}
                    sheet.Save(Path.Combine(output,"glyph_preview.png"),ImageFormat.Png);
                    File.WriteAllText(Path.Combine(output,"font_family.txt"),collection.Families[0].Name);
                    return result;
                }
            }
        } finally {RemoveFontResourceEx(path,16,IntPtr.Zero);}
    }
}
'@
$auditData = Get-Content -LiteralPath $Audit -Raw -Encoding UTF8 | ConvertFrom-Json
[int[]]$codes = @($auditData.placements | ForEach-Object { [Convert]::ToInt32($_.codepoint.Substring(2),16) })
if ($codes.Length -lt 1 -or $codes.Length -gt 11172 -or $codes.Length -ne $auditData.required_missing_hangul) {
    throw "Audited Hangul glyph count is invalid or incomplete"
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$metrics = [KoreanGlyphRasterizer]::Render((Resolve-Path -LiteralPath $Font).Path,$codes,(Resolve-Path -LiteralPath $OutputDirectory).Path)
$metrics | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'glyph_metrics.json') -Encoding UTF8
Write-Output "Rasterized $($metrics.Length) supplied-font Hangul glyphs into 24x24 cells."
