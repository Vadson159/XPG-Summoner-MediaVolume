# Shows the volume OSD (2 ticks down, then 2 up) and the track-info popup, and
# saves a PNG of the bottom-right corner of the primary screen where both sit,
# so font-rendering changes can be compared by eye.
param([Parameter(Mandatory)][string]$OutPrefix, [int]$InfoKey = 0x78)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\PerfCommon.ps1"
Add-Type -AssemblyName System.Drawing, System.Windows.Forms

function Grab([string]$path) {
    $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $w = 700; $h = 260
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($b.Right - $w, $b.Bottom - $h, 0, 0, $bmp.Size)
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
}

[void](Assert-OneInstance)
[SvmPerf.Native]::Tap($VK_VOLUME_DOWN); Start-Sleep -Milliseconds 15; [SvmPerf.Native]::Tap($VK_VOLUME_DOWN)
Start-Sleep -Milliseconds 400
Grab "$OutPrefix-osd.png"
[SvmPerf.Native]::Tap($VK_VOLUME_UP); Start-Sleep -Milliseconds 15; [SvmPerf.Native]::Tap($VK_VOLUME_UP)
Start-Sleep -Milliseconds 3500
[void](Assert-OneInstance)
[SvmPerf.Native]::Tap([uint16]$InfoKey)
Start-Sleep -Milliseconds 400
Grab "$OutPrefix-track.png"
