param([string]$Package,[string]$Ico,[switch]$IcoPlate,[string]$Preview)
# The app icon: the earbud outline from the app's battery row (chosen by the owner, 4 Nov 2026),
# on transparency, as the owner asked. Only the setup's .ico keeps a black square (-IcoPlate),
# because File Explorer shows it on white.
#   -Package <Assets>  every logo the MSIX needs. Windows draws the taskbar and Start icons without
#                      an accent-coloured plate only when "unplated" target-size variants exist and
#                      resources.pri indexes them (package.ps1 runs makepri), as in Tribit Connect.
#   -Ico <file>        an .ico for an executable's own icon (window title, taskbar previews, tray).
#   -Preview <folder>  Logo24.png and Logo150.png, to look at.
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
. (Join-Path $PSScriptRoot 'Convert-SvgPath.ps1')
# Same geometry as the left-earbud icon in ui/Dusk.xaml, on a 24-unit canvas.
$taskBud=@(
    'M18.09,10.63 L18.92,10.82 L19.72,11.15 L20.45,11.60 L21.11,12.15 L21.67,12.80 L22.13,13.53 L22.46,14.33 L22.67,15.16 L22.75,16.02 L22.69,16.88 L22.50,17.72 L22.18,18.52 L21.74,19.26 L21.20,19.92 L20.55,20.49 L19.83,20.95 L19.04,21.29 L18.21,21.51 L17.35,21.60 L16.49,21.55 L15.65,21.37 L14.85,21.06 L14.10,20.63',
    'M18.14,11.70 C18.13,12.41 18.05,13.14 17.89,13.85 C17.73,14.55 17.46,15.22 17.17,15.91 C16.88,16.60 16.56,17.28 16.15,17.98 C15.74,18.67 15.31,19.48 14.71,20.08 C14.10,20.68 13.32,21.32 12.51,21.57 C11.71,21.82 10.69,21.80 9.87,21.60 C9.04,21.39 8.23,20.81 7.55,20.35 C6.86,19.89 6.32,19.31 5.75,18.83 C5.18,18.34 4.64,17.92 4.14,17.42 C3.63,16.93 3.11,16.42 2.71,15.83 C2.30,15.24 1.97,14.57 1.73,13.88 C1.48,13.19 1.32,12.44 1.25,11.70 C1.18,10.95 1.17,10.15 1.30,9.40 C1.43,8.65 1.68,7.86 2.03,7.17 C2.38,6.47 2.87,5.81 3.41,5.23 C3.94,4.66 4.58,4.12 5.25,3.70 C5.92,3.27 6.68,2.90 7.45,2.69 C8.22,2.47 9.07,2.37 9.87,2.40 C10.66,2.43 11.48,2.60 12.24,2.85 C12.99,3.09 13.73,3.45 14.38,3.87 C15.03,4.30 15.65,4.84 16.14,5.42 C16.63,6.01 17.01,6.71 17.31,7.40 C17.61,8.08 17.79,8.82 17.93,9.54 C18.06,10.25 18.14,10.98 18.14,11.70 Z',
    'M3.99,11.16 A5.53,5.53 0 1 1 15.05,11.16 A5.53,5.53 0 1 1 3.99,11.16 Z')
# Light outline for dark surfaces; -Dark for the light taskbar and Start.
function New-AppIcon([int]$Size,[bool]$Plated,[bool]$Dark){
    $bitmap=[System.Drawing.Bitmap]::new($Size,$Size)
    $g=[System.Drawing.Graphics]::FromImage($bitmap)
    $g.SmoothingMode=[System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.PixelOffsetMode=[System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)
    # Unplated, the earbud fills the tile; plated, it sits inside the square's margin.
    $unit=if($Plated){$Size*4.1/160}else{$Size/24.0}
    if($Plated){
        $r=$Size*36.0/160;$d=2*$r;$e=$Size-$d
        $plate=[System.Drawing.Drawing2D.GraphicsPath]::new()
        $plate.AddArc(0,0,$d,$d,180,90);$plate.AddArc($e,0,$d,$d,270,90);$plate.AddArc($e,$e,$d,$d,0,90);$plate.AddArc(0,$e,$d,$d,90,90);$plate.CloseFigure()
        $fill=[System.Drawing.SolidBrush]::new([System.Drawing.Color]::Black);$g.FillPath($fill,$plate);$fill.Dispose();$plate.Dispose()
        $g.TranslateTransform([single]($Size*32.0/160),[single]($Size*30.0/160))
    }
    $g.ScaleTransform([single]$unit,[single]$unit)
    # Strokes stay at least about 1.4 px so the outline survives taskbar sizes.
    $pen=[System.Drawing.Pen]::new([System.Drawing.ColorTranslator]::FromHtml($(if($Dark){'#1C1C1C'}else{'#F2F2F2'})),[single][Math]::Max(1.2,1.4/$unit))
    $pen.StartCap=$pen.EndCap=[System.Drawing.Drawing2D.LineCap]::Round;$pen.LineJoin=[System.Drawing.Drawing2D.LineJoin]::Round
    foreach($data in $taskBud){$path=Convert-SvgPath $data;$g.DrawPath($pen,$path);$path.Dispose()}
    $pen.Dispose();$g.Dispose()
    return $bitmap
}
function Save-Icon([int]$Size,[string]$File,[bool]$Plated=$false,[bool]$Dark=$false){$b=New-AppIcon $Size $Plated $Dark;$b.Save($File,[System.Drawing.Imaging.ImageFormat]::Png);$b.Dispose()}
if($Package){
    New-Item -Path $Package -ItemType Directory -Force | Out-Null
    foreach($t in 16,20,24,30,32,36,40,48,60,64,72,80,96,256){
        Save-Icon $t (Join-Path $Package "Logo44.targetsize-$t.png")
        Save-Icon $t (Join-Path $Package "Logo44.targetsize-${t}_altform-unplated.png")
        Save-Icon $t (Join-Path $Package "Logo44.targetsize-${t}_altform-lightunplated.png") $false $true
    }
    foreach($s in @(@(100,1),@(125,1.25),@(150,1.5),@(200,2),@(400,4))){
        Save-Icon ([int](44*$s[1])) (Join-Path $Package "Logo44.scale-$($s[0]).png")
        Save-Icon ([int](150*$s[1])) (Join-Path $Package "Logo150.scale-$($s[0]).png")
    }
    # Unqualified copy for the widget picker and Public\Icon.png, which read the file directly.
    Save-Icon 50 (Join-Path $Package 'StoreLogo.png')
}
if($Preview){New-Item -Path $Preview -ItemType Directory -Force | Out-Null;foreach($t in 24,150){Save-Icon $t (Join-Path $Preview "Logo$t.png")}}
if($Ico){
    # Small frames are 32-bit bitmaps, which every icon reader accepts; 256 px is PNG.
    $frames=foreach($size in 16,20,24,32,40,48,64,256){
        $bmp=New-AppIcon $size $IcoPlate.IsPresent $false
        if($size -ge 256){$ms=[IO.MemoryStream]::new();$bmp.Save($ms,[System.Drawing.Imaging.ImageFormat]::Png);$bmp.Dispose();,@($size,$ms.ToArray());continue}
        $data=$bmp.LockBits([System.Drawing.Rectangle]::new(0,0,$size,$size),'ReadOnly',[System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $pixels=[byte[]]::new($data.Stride*$size);[Runtime.InteropServices.Marshal]::Copy($data.Scan0,$pixels,0,$pixels.Length);$bmp.UnlockBits($data);$bmp.Dispose()
        $mask=[int]([math]::Floor(($size+31)/32)*4)
        $ms=[IO.MemoryStream]::new();$w=[IO.BinaryWriter]::new($ms)
        $w.Write([uint32]40);$w.Write([int32]$size);$w.Write([int32](2*$size));$w.Write([uint16]1);$w.Write([uint16]32)
        $w.Write([uint32]0);$w.Write([uint32]($size*$size*4+$mask*$size));$w.Write([int32]0);$w.Write([int32]0);$w.Write([uint32]0);$w.Write([uint32]0)
        for($y=$size-1;$y -ge 0;$y--){$w.Write($pixels,$y*$data.Stride,$size*4)}
        $w.Write([byte[]]::new($mask*$size));$w.Flush()
        ,@($size,$ms.ToArray())
    }
    $out=[IO.MemoryStream]::new();$w=[IO.BinaryWriter]::new($out)
    $w.Write([uint16]0);$w.Write([uint16]1);$w.Write([uint16]$frames.Count)
    $offset=6+16*$frames.Count
    foreach($f in $frames){$s=$f[0];$w.Write([byte]($s%256));$w.Write([byte]($s%256));$w.Write([byte]0);$w.Write([byte]0);$w.Write([uint16]1);$w.Write([uint16]32);$w.Write([uint32]$f[1].Length);$w.Write([uint32]$offset);$offset+=$f[1].Length}
    foreach($f in $frames){$w.Write([byte[]]$f[1])}
    $w.Flush();[IO.File]::WriteAllBytes($Ico,$out.ToArray());$w.Dispose()
}
