<#
.SYNOPSIS
    Captures the app window and composites docs/screenshot.png, the hero image
    the README opens with.

.DESCRIPTION
    Pipeline:
      1. Launch the app and capture its window with PrintWindow.
      2. Trim the dead space below the footer.
      3. Scale the window to 1242x990 and composite it on a padded gradient at
         1432x1180, with a drop shadow and the window border redrawn.

    Three details here were arrived at the hard way and are easy to get wrong:

    PrintWindow renders from the GetWindowRect origin, which includes Windows'
    invisible resize border, so the capture has to be cropped by the difference
    between that rect and the visible frame. Skipping this puts a black bar down
    the left and silently clips the same width off the right.

    Comparing those two rectangles only works from a DPI-aware process.
    Otherwise GetWindowRect reports DPI-virtualised coordinates while DWM
    reports physical ones, and they disagree by the display scale rather than by
    the border width.

    The window border is drawn by the compositor, outside the window, so
    PrintWindow never captures it. It is redrawn here, half a pixel inward so
    the 1px stroke lands on the boundary column instead of straddling it into
    the shadow.

    Run this with the app in its default state: no ISO chosen, no checksum
    pasted. Launching a fresh instance gives that automatically, but the window
    size is remembered between runs, so a resized window will trip the aspect
    check below.

.EXAMPLE
    ./scripts/capture-screenshot.ps1

.EXAMPLE
    ./scripts/capture-screenshot.ps1 -QtBin C:/Qt/6.11.1/msvc2022_64/bin
#>

[CmdletBinding()]
param(
    [string]$ExePath,
    [string]$OutputPath,
    # Only needed when the Qt runtime is not already beside the executable or on
    # PATH. windeployqt normally puts it there as a post-build step.
    [string]$QtBin,
    [int]$LaunchWaitSeconds = 5,
    [switch]$KeepAppOpen
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$RepoRoot = Split-Path -Parent $PSScriptRoot
if (-not $ExePath)    { $ExePath    = Join-Path $RepoRoot "build/Release/iso-integrity-check.exe" }
if (-not $OutputPath) { $OutputPath = Join-Path $RepoRoot "docs/screenshot.png" }

# Canvas and window box, matching every previous release's hero image.
$CanvasW = 1432; $CanvasH = 1180
$WinX = 95; $WinY = 87; $WinW = 1242; $WinH = 990
$Radius = 9
# Least dead space to leave below the footer text, in output pixels.
$MinFooterPadding = 24
# Gradient endpoints, top-left to bottom-right.
$GradientFrom = [System.Drawing.Color]::FromArgb(0x20, 0x29, 0x39)
$GradientTo   = [System.Drawing.Color]::FromArgb(0x12, 0x17, 0x21)
# Shadow, matched against the 1.3.1 image by sampling its falloff down the left
# edge and below the window.
$ShadowSpread = 6; $ShadowOffsetY = 12; $ShadowBlur = 12; $ShadowPasses = 3; $ShadowOpacity = 0.95
$BorderColor = [System.Drawing.Color]::FromArgb(255, 85, 86, 87)

function Write-Step([string]$Message) {
    Write-Host ""
    Write-Host "==> $Message" -ForegroundColor Cyan
}

if (-not (Test-Path -LiteralPath $ExePath)) {
    throw "Executable not found at '$ExePath'. Build it first, or pass -ExePath."
}

if (-not ("IsoCapture" -as [type])) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public class IsoCapture {
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int attr, out RECT r, int size);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint from, uint to, bool attach);
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }

  public static void MakeDpiAware() {
    // PER_MONITOR_AWARE_V2, falling back to the older system-wide call.
    if (!SetProcessDpiAwarenessContext(new IntPtr(-4))) { SetProcessDPIAware(); }
  }

  // Windows refuses SetForegroundWindow from a process that does not already own
  // the foreground, and reports no error when it declines. Borrowing the current
  // foreground thread's input queue lifts that restriction; the return value is
  // the actual check.
  public static bool Activate(IntPtr h) {
    ShowWindow(h, 9);  // SW_RESTORE
    if (GetForegroundWindow() == h) { return true; }
    uint fgThread = GetWindowThreadProcessId(GetForegroundWindow(), IntPtr.Zero);
    uint self = GetCurrentThreadId();
    bool attached = fgThread != 0 && fgThread != self && AttachThreadInput(fgThread, self, true);
    SetForegroundWindow(h);
    BringWindowToTop(h);
    if (attached) { AttachThreadInput(fgThread, self, false); }
    return GetForegroundWindow() == h;
  }

  public static bool IsActive(IntPtr h) { return GetForegroundWindow() == h; }

  // Window rect (with the invisible resize border) then the visible frame.
  public static int[] Rects(IntPtr h) {
    RECT w, e;
    GetWindowRect(h, out w);
    DwmGetWindowAttribute(h, 9, out e, Marshal.SizeOf(typeof(RECT)));  // DWMWA_EXTENDED_FRAME_BOUNDS
    return new int[] { w.Left, w.Top, w.Right - w.Left, w.Bottom - w.Top,
                       e.Left, e.Top, e.Right - e.Left, e.Bottom - e.Top };
  }
}
'@
}

if (-not ("IsoShadow" -as [type])) {
    # Pure array maths, so the inline C# needs no System.Drawing reference.
    Add-Type @'
public class IsoShadow {
    public static byte[] BoxBlur(byte[] src, int w, int h, int r, int passes) {
        int[] cur = new int[w * h];
        for (int i = 0; i < w * h; i++) { cur[i] = src[i]; }
        int[] tmp = new int[w * h];
        int win = r * 2 + 1;
        for (int p = 0; p < passes; p++) {
            for (int y = 0; y < h; y++) {
                int row = y * w, sum = 0;
                for (int x = -r; x <= r; x++) { sum += cur[row + (x < 0 ? 0 : (x >= w ? w - 1 : x))]; }
                for (int x = 0; x < w; x++) {
                    tmp[row + x] = sum / win;
                    int add = x + r + 1; if (add >= w) { add = w - 1; }
                    int sub = x - r;     if (sub < 0)  { sub = 0; }
                    sum += cur[row + add] - cur[row + sub];
                }
            }
            for (int x = 0; x < w; x++) {
                int sum = 0;
                for (int y = -r; y <= r; y++) { sum += tmp[(y < 0 ? 0 : (y >= h ? h - 1 : y)) * w + x]; }
                for (int y = 0; y < h; y++) {
                    cur[y * w + x] = sum / win;
                    int add = y + r + 1; if (add >= h) { add = h - 1; }
                    int sub = y - r;     if (sub < 0)  { sub = 0; }
                    sum += tmp[add * w + x] - tmp[sub * w + x];
                }
            }
        }
        byte[] outp = new byte[w * h];
        for (int i = 0; i < w * h; i++) { int v = cur[i]; outp[i] = (byte)(v > 255 ? 255 : v); }
        return outp;
    }

    public static void Apply(byte[] bgra, byte[] a, int w, int h, double opacity) {
        for (int i = 0; i < w * h; i++) {
            double k = 1.0 - (a[i] / 255.0) * opacity;
            int o = i * 4;
            bgra[o]     = (byte)(bgra[o]     * k);
            bgra[o + 1] = (byte)(bgra[o + 1] * k);
            bgra[o + 2] = (byte)(bgra[o + 2] * k);
        }
    }

    // Lowest row holding light-on-dark text, used to find the footer.
    public static int LastTextRow(byte[] bgra, int w, int h, int inset) {
        for (int y = h - 1 - inset; y >= 0; y--) {
            int row = y * w * 4;
            for (int x = inset; x < w - inset; x++) {
                int o = row + x * 4;
                if (bgra[o] > 70 && bgra[o + 1] > 70 && bgra[o + 2] > 70) { return y; }
            }
        }
        return -1;
    }
}
'@
}

function New-RoundedPath([single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

function Get-BitmapBytes([System.Drawing.Bitmap]$bmp) {
    $rect = New-Object System.Drawing.Rectangle(0, 0, $bmp.Width, $bmp.Height)
    $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $bytes = New-Object byte[] ($bmp.Width * $bmp.Height * 4)
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
    $bmp.UnlockBits($data)
    return $bytes
}

[IsoCapture]::MakeDpiAware()

if ($QtBin) { $env:PATH = "$QtBin;$env:PATH" }

Write-Step "Launching the app"
$proc = Start-Process -FilePath $ExePath -PassThru
try {
    Start-Sleep -Seconds $LaunchWaitSeconds
    $proc.Refresh()
    if ($proc.HasExited) { throw "The app exited immediately. Is the Qt runtime beside it? Try -QtBin." }
    $handle = $proc.MainWindowHandle
    if ($handle -eq [IntPtr]::Zero) { throw "The app has no window yet. Try a larger -LaunchWaitSeconds." }
    # An unfocused window paints a greyed-out title bar. That is a real
    # difference from every previous release's image and nothing later in the
    # pipeline would catch it, so activation is verified rather than assumed.
    $activated = $false
    for ($attempt = 1; $attempt -le 10; $attempt++) {
        if ([IsoCapture]::Activate($handle)) { $activated = $true; break }
        Start-Sleep -Milliseconds 300
    }
    if (-not $activated) {
        throw "Could not bring the app window to the foreground; its title bar would be captured greyed out. Close whatever is holding focus and retry."
    }
    Start-Sleep -Seconds 2
    if (-not [IsoCapture]::IsActive($handle)) {
        throw "The app window lost focus before the capture. Retry without touching other windows."
    }

    Write-Step "Capturing the window"
    $r = [IsoCapture]::Rects($handle)
    $borderX = $r[4] - $r[0]
    $borderY = $r[5] - $r[1]
    Write-Host "  window rect  : $($r[2]) x $($r[3])"
    Write-Host "  visible frame: $($r[6]) x $($r[7])  (border offset $borderX,$borderY)"

    $full = New-Object System.Drawing.Bitmap($r[2], $r[3], [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $fg = [System.Drawing.Graphics]::FromImage($full)
    $hdc = $fg.GetHdc()
    [void][IsoCapture]::PrintWindow($handle, $hdc, 2)  # PW_RENDERFULLCONTENT
    $fg.ReleaseHdc($hdc); $fg.Dispose()

    $crop = New-Object System.Drawing.Rectangle($borderX, $borderY, $r[6], $r[7])
    $raw = $full.Clone($crop, $full.PixelFormat)
    $full.Dispose()
}
finally {
    if (-not $KeepAppOpen -and $proc -and -not $proc.HasExited) {
        [void]$proc.CloseMainWindow()
        Start-Sleep -Seconds 1
    }
}

# PrintWindow leaves a 1px black outline on the left, right and bottom. The top
# row is real title bar, so only the sides and bottom are skipped.
$inset = 1
$srcW = $raw.Width - (2 * $inset)

Write-Step "Trimming and compositing"
$rawBytes = Get-BitmapBytes $raw
$lastText = [IsoShadow]::LastTextRow($rawBytes, $raw.Width, $raw.Height, 40)
if ($lastText -lt 0) { throw "Found no footer text in the capture; the window may not have painted." }

# Trim to the canonical aspect so every release's image is the same shape.
$trim = [int][Math]::Round($srcW / ($WinW / [double]$WinH))
$scale = $WinW / [double]$srcW
$padding = [int][Math]::Round(($trim - $lastText) * $scale)
Write-Host "  footer text ends at row $lastText, trimming to $trim (padding ${padding}px in output)"
if ($trim -gt $raw.Height) {
    throw "The window is too short for the target aspect ($trim > $($raw.Height)). Make the window taller and retry."
}
if ($padding -lt $MinFooterPadding) {
    throw "Trimming to the target aspect would leave only ${padding}px below the footer (want $MinFooterPadding+). Make the window narrower or taller and retry."
}

$canvas = New-Object System.Drawing.Bitmap($CanvasW, $CanvasH, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($canvas)
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$canvasRect = New-Object System.Drawing.Rectangle(0, 0, $CanvasW, $CanvasH)
$brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush($canvasRect, $GradientFrom, $GradientTo, 45.0)
$g.FillRectangle($brush, $canvasRect)
$brush.Dispose(); $g.Dispose()

# Shadow: the window silhouette, spread and pushed down, blurred, then used to
# darken the canvas toward black.
$mask = New-Object System.Drawing.Bitmap($CanvasW, $CanvasH, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$mg = [System.Drawing.Graphics]::FromImage($mask)
$mg.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$mg.Clear([System.Drawing.Color]::Black)
$shadowPath = New-RoundedPath ($WinX - $ShadowSpread) ($WinY + $ShadowOffsetY - $ShadowSpread) `
    ($WinW + 2 * $ShadowSpread) ($WinH + 2 * $ShadowSpread) ($Radius + $ShadowSpread)
$mg.FillPath([System.Drawing.Brushes]::White, $shadowPath)
$shadowPath.Dispose(); $mg.Dispose()

$maskBytes = Get-BitmapBytes $mask
$mask.Dispose()
$alpha = New-Object byte[] ($CanvasW * $CanvasH)
for ($i = 0; $i -lt $CanvasW * $CanvasH; $i++) { $alpha[$i] = $maskBytes[$i * 4] }
$alpha = [IsoShadow]::BoxBlur($alpha, $CanvasW, $CanvasH, $ShadowBlur, $ShadowPasses)

$cData = $canvas.LockBits($canvasRect, [System.Drawing.Imaging.ImageLockMode]::ReadWrite,
    [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$cBytes = New-Object byte[] ($CanvasW * $CanvasH * 4)
[System.Runtime.InteropServices.Marshal]::Copy($cData.Scan0, $cBytes, 0, $cBytes.Length)
[IsoShadow]::Apply($cBytes, $alpha, $CanvasW, $CanvasH, $ShadowOpacity)
[System.Runtime.InteropServices.Marshal]::Copy($cBytes, 0, $cData.Scan0, $cBytes.Length)
$canvas.UnlockBits($cData)

$g = [System.Drawing.Graphics]::FromImage($canvas)
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality

$path = New-RoundedPath $WinX $WinY $WinW $WinH $Radius
$g.SetClip($path)
# Built up front: inside a New-Object argument list, PowerShell's comma parsing
# swallows the arithmetic and hands the constructor an array.
$dstRect = New-Object System.Drawing.Rectangle($WinX, $WinY, $WinW, $WinH)
$srcRect = New-Object System.Drawing.Rectangle($inset, 0, $srcW, $trim)
$g.DrawImage($raw, $dstRect, $srcRect, [System.Drawing.GraphicsUnit]::Pixel)
$g.ResetClip()

$borderPath = New-RoundedPath ($WinX + 0.5) ($WinY + 0.5) ($WinW - 1) ($WinH - 1) $Radius
$pen = New-Object System.Drawing.Pen($BorderColor, 1.0)
$g.DrawPath($pen, $borderPath)
$pen.Dispose(); $borderPath.Dispose(); $path.Dispose(); $g.Dispose()

$outDir = Split-Path -Parent $OutputPath
if ($outDir -and -not (Test-Path -LiteralPath $outDir)) {
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null
}
$canvas.Save($OutputPath, [System.Drawing.Imaging.ImageFormat]::Png)
$canvas.Dispose(); $raw.Dispose()

Write-Step "Done"
Write-Host "Wrote $OutputPath ($CanvasW x $CanvasH)" -ForegroundColor Green
