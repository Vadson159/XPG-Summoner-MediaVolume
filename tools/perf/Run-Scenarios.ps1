<#
.SYNOPSIS
  Runs the OPTIMIZATION_PLAN.md stage-0 scenarios against a SVM_PERF build.

.DESCRIPTION
  S1  cold start, then 60 s idle          (restarts the exe once per run)
  S2  volume nudge after >30 s idle       (2 ticks down, then 2 up to restore)
  S3  continuous scrolling for 3 s        (alternating 10 down / 10 up, net zero)
  S4  track-info key after >30 s idle
  S5  settings menu open, mouse moving 5 s
  S6  10 minutes idle

  S2 and S4 are interleaved (they both need a released device), so asking for
  either runs both.

  Safety: injects only Volume Up/Down, the menu key and the track-info key, and
  only while exactly one SmartVolumeMixer is running (no instance = the volume
  keys would move the system volume). Never touches Mute, Alt+Q or Alt+S.

  Results are appended as one JSON object per scenario to
  <exe dir>\perf-results.jsonl and printed as a summary.
#>
param(
    [Parameter(Mandatory)] [string]$Exe,
    [string[]]$Scenarios = @('S1', 'S2', 'S3', 'S5', 'S6'),
    [int]$Runs = 5,
    [string]$Label = 'run',
    [int]$IdleSeconds = 38,
    [int]$S6Minutes = 10,
    [int]$MenuKey = 0x77,
    [int]$InfoKey = 0x78
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\PerfCommon.ps1"

$Exe = (Resolve-Path $Exe).Path
$ExeDir = Split-Path $Exe
$PerfLog = Join-Path $ExeDir 'perf.log'
$ResultsFile = Join-Path $ExeDir 'perf-results.jsonl'

function Save-Result($obj) {
    $obj | Add-Member -NotePropertyName Label -NotePropertyValue $Label -Force
    $obj | Add-Member -NotePropertyName At -NotePropertyValue (Get-Date).ToString('s') -Force
    $json = $obj | ConvertTo-Json -Compress -Depth 6
    # Indexers / AV can hold the file for a moment; never lose a run to that.
    for ($i = 0; $i -lt 20; ++$i) {
        try { [IO.File]::AppendAllText($ResultsFile, $json + "`n"); break } catch { Start-Sleep -Milliseconds 250 }
    }
    # Printed here (Write-Host) rather than returned, so a later failure in the
    # same invocation cannot swallow it.
    Write-Host ($obj | Select-Object * -ExcludeProperty ModuleNames, Samples, RawOsd, RawTrack, ThreadMcycles | Format-List | Out-String -Width 200)
}

function Get-LogLines {
    if (Test-Path $PerfLog) { return @(Get-Content $PerfLog) } else { return @() }
}

# perf.log is appended by the render loop's 5 s tray timer, so new lines lag by
# up to 5 s. Waits for that flush and returns the lines added since $From.
function Get-NewLines([int]$From) {
    Start-Sleep -Milliseconds 6000
    $all = Get-LogLines
    if ($all.Count -le $From) { return @() }
    return $all[$From..($all.Count - 1)]
}

function Parse-Kv([string]$line) {
    $h = @{}
    foreach ($m in [regex]::Matches($line, '(\w+)=(-?[\d\.]+)')) { $h[$m.Groups[1].Value] = [double]$m.Groups[2].Value }
    return $h
}

function Get-Timings([string[]]$lines, [string]$name) {
    @($lines | Where-Object { $_ -match "^\s*[\d\.]+ T $([regex]::Escape($name)) ([\d\.]+)$" } | ForEach-Object { [double]$Matches[1] })
}

function Stop-Svm {
    foreach ($p in Get-SvmProcess) {
        [void][SvmPerf.Native]::QuitInstance($p.Id)
        if (-not $p.WaitForExit(15000)) { Stop-Process -Id $p.Id -Force }
    }
}

function Start-Svm {
    if ((Get-SvmProcess).Count -ne 0) { throw 'An instance is already running.' }
    $p = Start-Process -FilePath $Exe -WorkingDirectory $ExeDir -PassThru
    Start-Sleep -Milliseconds 1500
    return $p
}

function Ensure-Running {
    $p = Get-SvmProcess
    if ($p.Count -eq 1) {
        if ($p[0].Path -ne $Exe) { throw "Running instance is $($p[0].Path), not $Exe" }
        return $p[0]
    }
    if ($p.Count -gt 1) { throw 'More than one instance running.' }
    return Start-Svm
}

function Tap([int]$vk, [int]$gapMs = 0) {
    [SvmPerf.Native]::Tap([uint16]$vk)
    if ($gapMs) { Start-Sleep -Milliseconds $gapMs }
}

# ---------------------------------------------------------------------------
function Run-S1 {
    $samples = @()
    for ($r = 1; $r -le $Runs; ++$r) {
        Stop-Svm
        Start-Sleep -Milliseconds 1000
        $from = (Get-LogLines).Count
        $p = Start-Svm
        Start-Sleep -Seconds 60
        $snap = Get-SvmSnapshot $p.Id
        $lines = Get-NewLines $from
        $kv = @{}
        foreach ($l in $lines) {
            if ($l -match '^\s*[\d\.]+ S (\S+) ([\d\.]+)$') { $kv[$Matches[1]] = [double]$Matches[2] }
            elseif ($l -match '^\s*[\d\.]+ T (start\.\S+|create\.\S+|fonts\.\S+) ([\d\.]+)$') { if (-not $kv.ContainsKey($Matches[1])) { $kv[$Matches[1]] = [double]$Matches[2] } }
        }
        $s = [pscustomobject]@{
            ToHookMs = $kv['process_start_to_hook']; ToLoopMs = $kv['process_start_to_loop']; ToAppMainMs = $kv['process_start_to_appmain']
            Phases = $kv; PeakWSMB = $snap.PeakWSMB; PrivateMB = $snap.PrivateMB; WorkingSetMB = $snap.WorkingSetMB
            Modules = $snap.Modules; Threads = $snap.Threads; Handles = $snap.Handles; GpuDedMB = $snap.GpuDedMB; GpuSharedMB = $snap.GpuSharedMB
            ModuleNames = $snap.ModuleNames
        }
        $samples += $s
        Write-Host ("S1 run {0}: hook {1:N1} ms, loop {2:N1} ms, peak WS {3} MB, private {4} MB, modules {5}, threads {6}, gpu {7}/{8} MB" -f $r, $s.ToHookMs, $s.ToLoopMs, $s.PeakWSMB, $s.PrivateMB, $s.Modules, $s.Threads, $s.GpuDedMB, $s.GpuSharedMB)
    }
    $res = [pscustomobject]@{
        Scenario = 'S1'
        ToHookMs = Format-Stat ($samples.ToHookMs) 'N1'
        ToLoopMs = Format-Stat ($samples.ToLoopMs) 'N1'
        PeakWSMB = Format-Stat ($samples.PeakWSMB) 'N1'
        PrivateMB = Format-Stat ($samples.PrivateMB) 'N2'
        Modules = Format-Stat ($samples.Modules) 'N0'
        Threads = Format-Stat ($samples.Threads) 'N0'
        Handles = Format-Stat ($samples.Handles) 'N0'
        GpuMB = Format-Stat ($samples | ForEach-Object { $_.GpuDedMB + $_.GpuSharedMB }) 'N1'
        CreateDevice = Format-Stat (@($samples | ForEach-Object { $_.Phases['create.D3D11CreateDevice'] } | Where-Object { $_ -ne $null })) 'N1'
        CreateFonts = Format-Stat (@($samples | ForEach-Object { $_.Phases['create.RebuildFonts'] } | Where-Object { $_ -ne $null })) 'N1'
        ConfigLoad = Format-Stat (@($samples | ForEach-Object { $_.Phases['start.Config.Load'] } | Where-Object { $_ -ne $null })) 'N2'
        AudioInit = Format-Stat (@($samples | ForEach-Object { $_.Phases['start.AudioManager.Initialize'] } | Where-Object { $_ -ne $null })) 'N2'
        HookInstall = Format-Stat (@($samples | ForEach-Object { $_.Phases['start.InputHook.Install'] } | Where-Object { $_ -ne $null })) 'N2'
        OverlayCreate = Format-Stat (@($samples | ForEach-Object { $_.Phases['start.OverlayWindow.Create'] } | Where-Object { $_ -ne $null })) 'N1'
        ModuleNames = $samples[-1].ModuleNames
        Samples = @($samples | Select-Object ToHookMs, ToLoopMs, PeakWSMB, PrivateMB, Modules, Threads, Handles)
    }
    Save-Result $res
}

function Run-S2S4 {
    $p = Ensure-Running
    $from = (Get-LogLines).Count
    for ($r = 1; $r -le $Runs; ++$r) {
        foreach ($kind in 'S2', 'S4') {
            Write-Host "$kind run $r : waiting $IdleSeconds s for the device to be released..."
            Start-Sleep -Seconds $IdleSeconds
            [void](Assert-OneInstance)
            if ($kind -eq 'S2') {
                Tap $VK_VOLUME_DOWN 15; Tap $VK_VOLUME_DOWN
                Start-Sleep -Milliseconds 1200
                Tap $VK_VOLUME_UP 15; Tap $VK_VOLUME_UP
            } else {
                Tap $InfoKey
            }
            Start-Sleep -Milliseconds 500
        }
    }
    $lines = Get-NewLines $from

    # The first E2E line after each idle_cleanup is the cold sample.
    # Every run waits $IdleSeconds first, so the very first E2E line is cold too
    # even though its idle_cleanup happened before $from.
    $osd = @(); $track = @(); $osdFire = @(); $cold = $true
    $recreate = @(); $chainCold = @(); $changeVol = @(); $trackInfo = @(); $swap = @()
    foreach ($l in $lines) {
        if ($l -match ' S idle_cleanup$') { $cold = $true; continue }
        if ($cold -and $l -match ' E2E osd ') { $kv = Parse-Kv $l; $osd += $kv['fromFirstTick']; $osdFire += $kv['fromFire']; $cold = $false }
        elseif ($cold -and $l -match ' E2E track ') { $kv = Parse-Kv $l; $track += $kv['fromKey']; $cold = $false }
    }
    $res = [pscustomobject]@{
        Scenario = 'S2+S4'
        S2_TickToOsdMs = Format-Stat $osd 'N1'
        S2_FireToOsdMs = Format-Stat $osdFire 'N1'
        S4_KeyToTrackMs = Format-Stat $track 'N1'
        RecreateTotalMs = Format-Stat (Get-Timings $lines 'recreate.total') 'N1'
        RecreateDeviceMs = Format-Stat (Get-Timings $lines 'recreate.D3D11CreateDevice') 'N1'
        RecreateImGuiInitMs = Format-Stat (Get-Timings $lines 'recreate.ImGuiDx11Init') 'N2'
        RecreateFontsMs = Format-Stat (Get-Timings $lines 'recreate.RebuildFonts') 'N1'
        AtlasBuildMs = Format-Stat (Get-Timings $lines 'fonts.AtlasBuild') 'N1'
        AtlasUploadMs = Format-Stat (Get-Timings $lines 'fonts.CreateDeviceObjects') 'N1'
        CreateSwapChainMs = Format-Stat (Get-Timings $lines 'frame.CreateSwapChain') 'N1'
        ChainColdMs = Format-Stat (Get-Timings $lines 'audio.GetSessionEnumerator.cold') 'N2'
        ChainWarmMs = Format-Stat (Get-Timings $lines 'audio.GetSessionEnumerator.warm') 'N2'
        ChangeVolumeMs = Format-Stat (Get-Timings $lines 'audio.ChangeVolume') 'N2'
        TrackInfoMs = Format-Stat (Get-Timings $lines 'audio.GetCurrentTrackInfo') 'N1'
        AtlasSize = [string](@($lines | Where-Object { $_ -match 'fonts.AtlasSize' }) | Select-Object -Last 1)
        RawOsd = $osd; RawTrack = $track
    }
    Save-Result $res
}

function Run-S3 {
    $p = Ensure-Running
    $samples = @()
    for ($r = 1; $r -le $Runs; ++$r) {
        # Warm up so the device exists, then let the debounce/stream state settle.
        Tap $VK_VOLUME_DOWN 15; Tap $VK_VOLUME_DOWN 15; Tap $VK_VOLUME_UP 15; Tap $VK_VOLUME_UP
        Start-Sleep -Seconds 4
        $from = (Get-LogLines).Count
        Start-Sleep -Seconds 2 # let the flush that may be pending land before the baseline
        $from = (Get-LogLines).Count
        $before = Get-SvmSnapshot $p.Id
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $ticks = 0; $dir = $VK_VOLUME_DOWN
        while ($sw.ElapsedMilliseconds -lt 3000) {
            [void](Assert-OneInstance)
            Tap $dir 30
            ++$ticks
            if ($ticks % 10 -eq 0) { $dir = if ($dir -eq $VK_VOLUME_DOWN) { $VK_VOLUME_UP } else { $VK_VOLUME_DOWN } }
        }
        # Balance an odd half-group so the net volume change is zero.
        $rem = $ticks % 20
        if ($rem -gt 0 -and $rem -le 10) { for ($i = 0; $i -lt $rem; ++$i) { Tap $VK_VOLUME_UP 30 } }
        elseif ($rem -gt 10) { for ($i = 0; $i -lt (20 - $rem); ++$i) { Tap $VK_VOLUME_DOWN 30 } }
        Start-Sleep -Milliseconds 3500 # OSD expires (2.5 s) and the clearing frame is drawn
        $after = Get-SvmSnapshot $p.Id
        $lines = Get-NewLines $from
        $f = @($lines | Where-Object { $_ -match ' F ' } | ForEach-Object { Parse-Kv $_ })
        $frames = $f.Count
        $avgTotal = if ($frames) { ($f | ForEach-Object { $_['total'] } | Measure-Object -Average).Average } else { 0 }
        $avgPresent = if ($frames) { ($f | ForEach-Object { $_['present'] } | Measure-Object -Average).Average } else { 0 }
        $mcyc = ($after.Cycles - $before.Cycles) / 1e6
        $s = [pscustomobject]@{ Ticks = $ticks; Frames = $frames; FrameMs = $avgTotal; PresentMs = $avgPresent; ProcMcycles = $mcyc; McyclesPerFrame = if ($frames) { $mcyc / $frames } else { 0 }; CtxSwitches = $after.CtxSwitches - $before.CtxSwitches }
        $samples += $s
        Write-Host ("S3 run {0}: {1} ticks, {2} frames, {3:N3} ms/frame (present {4:N3}), {5:N1} Mcycles total, {6:N2} Mcycles/frame" -f $r, $ticks, $frames, $avgTotal, $avgPresent, $mcyc, $s.McyclesPerFrame)
    }
    $res = [pscustomobject]@{
        Scenario = 'S3'
        Frames = Format-Stat ($samples.Frames) 'N0'
        FrameMs = Format-Stat ($samples.FrameMs) 'N3'
        PresentMs = Format-Stat ($samples.PresentMs) 'N3'
        ProcMcycles = Format-Stat ($samples.ProcMcycles) 'N1'
        McyclesPerFrame = Format-Stat ($samples.McyclesPerFrame) 'N2'
        CtxSwitches = Format-Stat ($samples.CtxSwitches) 'N0'
        Samples = $samples
    }
    Save-Result $res
}

function Run-S5 {
    $p = Ensure-Running
    $samples = @()
    Add-Type -AssemblyName System.Windows.Forms
    for ($r = 1; $r -le $Runs; ++$r) {
        $start = [System.Windows.Forms.Cursor]::Position
        Start-Sleep -Seconds 2
        $from = (Get-LogLines).Count
        [void](Assert-OneInstance)
        Tap $MenuKey
        Start-Sleep -Milliseconds 1000
        $before = Get-SvmSnapshot $p.Id
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $i = 0
        while ($sw.ElapsedMilliseconds -lt 5000) {
            $a = $i * 0.15
            [SvmPerf.Native]::MouseMove([int](6 * [math]::Cos($a)), [int](6 * [math]::Sin($a)))
            ++$i
            Start-Sleep -Milliseconds 16
        }
        $after = Get-SvmSnapshot $p.Id
        [void](Assert-OneInstance)
        Tap $MenuKey
        [System.Windows.Forms.Cursor]::Position = $start
        $lines = Get-NewLines $from
        # Frames inside the 5 s window only: drop the ones the open/close settle drew.
        $f = @($lines | Where-Object { $_ -match ' F ' } | ForEach-Object { Parse-Kv $_ })
        $mcyc = ($after.Cycles - $before.Cycles) / 1e6
        $avgTotal = if ($f.Count) { ($f | ForEach-Object { $_['total'] } | Measure-Object -Average).Average } else { 0 }
        $s = [pscustomobject]@{ FramesInclSettle = $f.Count; FrameMs = $avgTotal; ProcMcycles5s = $mcyc; Moves = $i }
        $samples += $s
        Write-Host ("S5 run {0}: {1} frames (incl. open/close), {2:N3} ms/frame, {3:N1} Mcycles in 5 s" -f $r, $f.Count, $avgTotal, $mcyc)
    }
    $res = [pscustomobject]@{
        Scenario = 'S5'
        Frames = Format-Stat ($samples.FramesInclSettle) 'N0'
        FrameMs = Format-Stat ($samples.FrameMs) 'N3'
        ProcMcycles5s = Format-Stat ($samples.ProcMcycles5s) 'N1'
        Samples = $samples
    }
    Save-Result $res
}

function Run-S6 {
    $p = Ensure-Running
    # Same starting point every time: the app has just been used (a volume
    # nudge and a track-info request, which bring up the WASAPI and SMTC
    # caches), then is left alone.
    [void](Assert-OneInstance)
    Tap $VK_VOLUME_DOWN 15; Tap $VK_VOLUME_DOWN; Start-Sleep -Milliseconds 800
    Tap $VK_VOLUME_UP 15; Tap $VK_VOLUME_UP; Start-Sleep -Milliseconds 3000
    Tap $InfoKey
    Write-Host "S6: settling 40 s, then $S6Minutes min idle..."
    Start-Sleep -Seconds 40
    $before = Get-SvmSnapshot $p.Id
    Start-Sleep -Seconds ($S6Minutes * 60)
    $after = Get-SvmSnapshot $p.Id
    $threadDeltas = foreach ($t in $after.ThreadCycles) {
        $b = $before.ThreadCycles | Where-Object { $_.Id -eq $t.Id }
        [pscustomobject]@{ Id = $t.Id; Priority = $t.Priority; Mcycles = [math]::Round(($t.Cycles - $(if ($b) { $b.Cycles } else { 0 })) / 1e6, 2) }
    }
    $res = [pscustomobject]@{
        Scenario = 'S6'
        Minutes = $S6Minutes
        ProcMcycles = [math]::Round(($after.Cycles - $before.Cycles) / 1e6, 2)
        CtxSwitches = $after.CtxSwitches - $before.CtxSwitches
        Threads = $after.Threads
        PrivateMB = $after.PrivateMB
        WorkingSetMB = $after.WorkingSetMB
        GpuMB = $after.GpuDedMB + $after.GpuSharedMB
        Handles = $after.Handles
        ThreadMcycles = $threadDeltas
    }
    Save-Result $res
}

$done = @{}
foreach ($sc in $Scenarios) {
    switch ($sc) {
        'S1' { Run-S1 }
        { $_ -in 'S2', 'S4' } { if (-not $done['S24']) { Run-S2S4; $done['S24'] = $true } }
        'S3' { Run-S3 }
        'S5' { Run-S5 }
        'S6' { Run-S6 }
        default { throw "Unknown scenario $sc" }
    }
}
