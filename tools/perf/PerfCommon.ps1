# Shared helpers for the SmartVolumeMixer perf scripts (OPTIMIZATION_PLAN.md, stage 0).
# Dot-source this file: . "$PSScriptRoot\PerfCommon.ps1"

if (-not ('SvmPerf.Native' -as [type])) {
    Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

namespace SvmPerf {
public static class Native {
    [StructLayout(LayoutKind.Sequential)]
    public struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Sequential)]
    public struct MOUSEINPUT { public int dx; public int dy; public uint mouseData; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Explicit)]
    public struct INPUTUNION { [FieldOffset(0)] public MOUSEINPUT mi; [FieldOffset(0)] public KEYBDINPUT ki; }
    [StructLayout(LayoutKind.Sequential)]
    public struct INPUT { public uint type; public INPUTUNION u; }

    [DllImport("user32.dll", SetLastError = true)]
    static extern uint SendInput(uint n, INPUT[] inputs, int size);

    const uint KEYEVENTF_EXTENDEDKEY = 1, KEYEVENTF_KEYUP = 2, MOUSEEVENTF_MOVE = 1;

    public static void Key(ushort vk, bool up) {
        var i = new INPUT[1];
        i[0].type = 1;
        i[0].u.ki.wVk = vk;
        uint flags = up ? KEYEVENTF_KEYUP : 0;
        // Media/volume keys are extended keys on a real keyboard.
        if (vk >= 0xAD && vk <= 0xB7) flags |= KEYEVENTF_EXTENDEDKEY;
        i[0].u.ki.dwFlags = flags;
        SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
    }
    public static void Tap(ushort vk) { Key(vk, false); Key(vk, true); }

    public static void MouseMove(int dx, int dy) {
        var i = new INPUT[1];
        i[0].type = 0;
        i[0].u.mi.dx = dx; i[0].u.mi.dy = dy; i[0].u.mi.dwFlags = MOUSEEVENTF_MOVE;
        SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
    }

    [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll")] static extern IntPtr OpenThread(uint access, bool inherit, int tid);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")] static extern bool QueryProcessCycleTime(IntPtr h, out ulong cycles);
    [DllImport("kernel32.dll")] static extern bool QueryThreadCycleTime(IntPtr h, out ulong cycles);
    [DllImport("kernel32.dll")] static extern bool GetProcessHandleCount(IntPtr h, out uint count);

    [StructLayout(LayoutKind.Sequential)]
    public struct PMC_EX { public uint cb; public uint PageFaultCount; public UIntPtr PeakWorkingSetSize; public UIntPtr WorkingSetSize;
        public UIntPtr QuotaPeakPagedPoolUsage; public UIntPtr QuotaPagedPoolUsage; public UIntPtr QuotaPeakNonPagedPoolUsage; public UIntPtr QuotaNonPagedPoolUsage;
        public UIntPtr PagefileUsage; public UIntPtr PeakPagefileUsage; public UIntPtr PrivateUsage; }
    [DllImport("psapi.dll")] static extern bool GetProcessMemoryInfo(IntPtr h, out PMC_EX pmc, uint cb);

    const uint PROCESS_QUERY_LIMITED_INFORMATION = 0x1000, PROCESS_VM_READ = 0x10, THREAD_QUERY_LIMITED_INFORMATION = 0x800;

    public static ulong ProcessCycles(int pid) {
        IntPtr h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid);
        if (h == IntPtr.Zero) return 0;
        ulong c; QueryProcessCycleTime(h, out c); CloseHandle(h); return c;
    }
    public static ulong ThreadCycles(int tid) {
        IntPtr h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, false, tid);
        if (h == IntPtr.Zero) return 0;
        ulong c; QueryThreadCycleTime(h, out c); CloseHandle(h); return c;
    }
    public static PMC_EX Memory(int pid) {
        PMC_EX m = new PMC_EX(); m.cb = (uint)Marshal.SizeOf(typeof(PMC_EX));
        IntPtr h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, false, pid);
        if (h == IntPtr.Zero) return m;
        GetProcessMemoryInfo(h, out m, m.cb); CloseHandle(h); return m;
    }

    // Cumulative context switches per thread, from the kernel's process list.
    // Every wake of a sleeping thread is at least one switch in, so the sum is
    // an upper bound on wake-ups that works on any build, perf or not.
    [DllImport("ntdll.dll")] static extern int NtQuerySystemInformation(int cls, IntPtr buf, int len, out int ret);
    public static long ContextSwitches(int pid) {
        int len = 1 << 20;
        for (int attempt = 0; attempt < 6; ++attempt, len *= 2) {
            IntPtr buf = Marshal.AllocHGlobal(len);
            try {
                int ret;
                int st = NtQuerySystemInformation(5, buf, len, out ret);
                if (st == unchecked((int)0xC0000004)) continue; // STATUS_INFO_LENGTH_MISMATCH
                if (st != 0) return -1;
                long off = 0;
                while (true) {
                    IntPtr p = new IntPtr(buf.ToInt64() + off);
                    int next = Marshal.ReadInt32(p, 0);
                    int nThreads = Marshal.ReadInt32(p, 4);
                    long id = Marshal.ReadIntPtr(p, 0x50).ToInt64();
                    if (id == pid) {
                        long sum = 0;
                        for (int t = 0; t < nThreads; ++t)
                            sum += (uint)Marshal.ReadInt32(p, 0x100 + t * 0x50 + 0x40);
                        return sum;
                    }
                    if (next == 0) return -1;
                    off += next;
                }
            } finally { Marshal.FreeHGlobal(buf); }
        }
        return -1;
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, string title);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")] static extern bool PostThreadMessageW(uint tid, uint msg, IntPtr w, IntPtr l);

    // Clean exit: WM_QUIT to the overlay's (render) thread, which is exactly
    // what the tray menu's Exit does (PostQuitMessage on that thread).
    public static bool QuitInstance(int pid) {
        IntPtr hwnd = IntPtr.Zero;
        while ((hwnd = FindWindowExW(IntPtr.Zero, hwnd, "SmartMixerClass", null)) != IntPtr.Zero) {
            uint wpid; uint tid = GetWindowThreadProcessId(hwnd, out wpid);
            if (wpid == (uint)pid) return PostThreadMessageW(tid, 0x0012, IntPtr.Zero, IntPtr.Zero);
        }
        return false;
    }
}
}
'@
}

$VK_VOLUME_DOWN = 0xAE
$VK_VOLUME_UP = 0xAF
$VK_F8 = 0x77
$VK_F9 = 0x78

function Get-SvmProcess {
    @(Get-Process -Name SmartVolumeMixer -ErrorAction SilentlyContinue)
}

# Refuses to inject anything unless exactly one instance is running: with none,
# the volume keys would reach Windows and move the SYSTEM volume; with two, every
# tick is applied twice.
function Assert-OneInstance {
    $p = Get-SvmProcess
    if ($p.Count -ne 1) { throw "Expected exactly one SmartVolumeMixer, found $($p.Count). Refusing to inject." }
    return $p[0]
}

function Get-SvmSnapshot([int]$ProcessId) {
    $p = Get-Process -Id $ProcessId -ErrorAction Stop
    $m = [SvmPerf.Native]::Memory($ProcessId)
    $threads = @($p.Threads | ForEach-Object {
        [pscustomobject]@{ Id = $_.Id; Cycles = [SvmPerf.Native]::ThreadCycles($_.Id); Priority = $_.PriorityLevel.ToString() }
    })
    $gpuDed = 0; $gpuShared = 0
    try {
        $c = Get-Counter -Counter "\GPU Process Memory(pid_$($ProcessId)_*)\Dedicated Usage", "\GPU Process Memory(pid_$($ProcessId)_*)\Shared Usage" -ErrorAction Stop
        foreach ($s in $c.CounterSamples) {
            if ($s.Path -like '*dedicated usage') { $gpuDed += $s.CookedValue } else { $gpuShared += $s.CookedValue }
        }
    } catch { }
    [pscustomobject]@{
        Time         = Get-Date
        PrivateMB    = [math]::Round([double]$m.PrivateUsage.ToUInt64() / 1MB, 2)
        WorkingSetMB = [math]::Round([double]$m.WorkingSetSize.ToUInt64() / 1MB, 2)
        PeakWSMB     = [math]::Round([double]$m.PeakWorkingSetSize.ToUInt64() / 1MB, 2)
        Handles      = $p.HandleCount
        Threads      = $p.Threads.Count
        Modules      = @($p.Modules).Count
        ModuleNames  = @($p.Modules | ForEach-Object { $_.ModuleName })
        Cycles       = [SvmPerf.Native]::ProcessCycles($ProcessId)
        CtxSwitches  = [SvmPerf.Native]::ContextSwitches($ProcessId)
        ThreadCycles = $threads
        GpuDedMB     = [math]::Round($gpuDed / 1MB, 2)
        GpuSharedMB  = [math]::Round($gpuShared / 1MB, 2)
    }
}

function Get-Median([double[]]$Values) {
    if (-not $Values -or $Values.Count -eq 0) { return [double]::NaN }
    $s = $Values | Sort-Object
    $n = $s.Count
    if ($n % 2) { return [double]$s[($n - 1) / 2] }
    return ([double]$s[$n / 2 - 1] + [double]$s[$n / 2]) / 2
}

function Format-Stat([double[]]$Values, [string]$Fmt = 'N1') {
    if (-not $Values -or $Values.Count -eq 0) { return 'n/a' }
    $med = Get-Median $Values
    $min = ($Values | Measure-Object -Minimum).Minimum
    $max = ($Values | Measure-Object -Maximum).Maximum
    return ("{0} ({1}..{2}, n={3})" -f $med.ToString($Fmt), ([double]$min).ToString($Fmt), ([double]$max).ToString($Fmt), $Values.Count)
}
