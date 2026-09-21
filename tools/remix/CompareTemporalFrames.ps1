param(
    [Parameter(Mandatory)][string]$Pattern,
    [string]$Directory = 'J:/hdresreach/skyrim-community-shaders/.research/flicker'
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
if (!('RemixTemporalPatch' -as [type])) {
    $references = @([AppContext]::GetData('TRUSTED_PLATFORM_ASSEMBLIES') -split [IO.Path]::PathSeparator)
    $references += [System.Drawing.Bitmap].Assembly.Location
    Add-Type -ReferencedAssemblies $references -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class RemixTemporalPatch {
    public static float[] Read(string path, out int width, out int height) {
        using (var source = new Bitmap(path))
        using (var b = new Bitmap(source.Width, source.Height, PixelFormat.Format32bppArgb)) {
            using (var g = Graphics.FromImage(b)) g.DrawImageUnscaled(source, 0, 0);
            width=b.Width; height=b.Height;
            var data=b.LockBits(new Rectangle(0,0,width,height),ImageLockMode.ReadOnly,PixelFormat.Format32bppArgb);
            try {
                var bytes=new byte[data.Stride*height]; Marshal.Copy(data.Scan0,bytes,0,bytes.Length);
                var pixels=new float[width*height];
                for(int y=0;y<height;y++) for(int x=0;x<width;x++) {
                    int p=y*data.Stride+x*4;
                    pixels[y*width+x]=(float)(bytes[p]*0.0722+bytes[p+1]*0.7152+bytes[p+2]*0.2126);
                }
                return pixels;
            } finally { b.UnlockBits(data); }
        }
    }
    public static double[] Compare(float[] a,float[] b,int width,int x0,int y0,int w,int h) {
        double best=double.MaxValue, zero=0, meanA=0,meanB=0; int bx=0,by=0,count=0;
        for(int y=y0;y<y0+h;y+=2) for(int x=x0;x<x0+w;x+=2) {
            meanA+=a[y*width+x]; meanB+=b[y*width+x]; count++;
        }
        meanA/=count; meanB/=count;
        for(int dy=-3;dy<=3;dy++) for(int dx=-3;dx<=3;dx++) {
            double error=0;
            for(int y=y0;y<y0+h;y+=2) for(int x=x0;x<x0+w;x+=2)
                error+=Math.Abs((a[y*width+x]-meanA)-(b[(y+dy)*width+x+dx]-meanB));
            error/=count;
            if(dx==0 && dy==0) zero=error;
            if(error<best) {best=error;bx=dx;by=dy;}
        }
        return new double[]{meanA,meanB,zero,best,bx,by};
    }
}
'@
}
# Regions apply only to the documented fixed Riverwood view at
# (17224.77,-47204.45,30), pitch/yaw 0, 1920x1080. Exclude HUD and vegetation.
# This is a diagnostic, not a pass/fail gate for general scene stability.
$regions = @(
    @{name='cliff';x=620;y=300;w=190;h=160},
    @{name='foregroundRock';x=1150;y=810;w=190;h=150},
    @{name='building';x=1770;y=485;w=100;h=90}
)
$files = @(Get-ChildItem -LiteralPath $Directory -Filter $Pattern | Sort-Object Name)
if ($files.Count -lt 2) { throw 'At least two frames are required.' }
$width=0; $height=0
$previous=[RemixTemporalPatch]::Read($files[0].FullName,[ref]$width,[ref]$height)
if ($width -ne 1920 -or $height -ne 1080) { throw 'Regions require 1920x1080 captures.' }
$rows=for($i=1;$i -lt $files.Count;$i++) {
    $current=[RemixTemporalPatch]::Read($files[$i].FullName,[ref]$width,[ref]$height)
    if($width -ne 1920 -or $height -ne 1080) {throw 'Frame dimensions changed.'}
    foreach($r in $regions) {
        $v=[RemixTemporalPatch]::Compare($previous,$current,$width,$r.x,$r.y,$r.w,$r.h)
        [ordered]@{frame=$files[$i].Name;region=$r.name;mean=$v[1];brightnessStep=$v[1]-$v[0];
            centeredMad=$v[2];alignedMad=$v[3];shiftX=$v[4];shiftY=$v[5]}
    }
    $previous=$current
}
[ordered]@{pattern=$Pattern;frames=$files.Count;scope='Fixed-view patches, mean-centered luminance, integer shift search +/-3 pixels; not motion/parity validation';rows=@($rows)} | ConvertTo-Json -Depth 5
