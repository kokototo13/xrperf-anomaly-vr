# Waits for AnomalyDX11*.exe to start and prints its integrity level (Medium = normal, High = admin).
# Used to check whether the launcher runs the game elevated.
param([int]$TimeoutSec = 600)

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Integrity {
    [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("advapi32.dll", SetLastError=true)] static extern bool OpenProcessToken(IntPtr proc, uint access, out IntPtr token);
    [DllImport("advapi32.dll", SetLastError=true)] static extern bool GetTokenInformation(IntPtr token, int cls, IntPtr info, int len, out int retLen);
    [DllImport("advapi32.dll")] static extern IntPtr GetSidSubAuthority(IntPtr sid, uint index);
    [DllImport("advapi32.dll")] static extern IntPtr GetSidSubAuthorityCount(IntPtr sid);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    public static string Get(int pid) {
        IntPtr p = OpenProcess(0x1000, false, pid); // PROCESS_QUERY_LIMITED_INFORMATION
        if (p == IntPtr.Zero) return "cannot open process (error " + Marshal.GetLastWin32Error() + ")";
        IntPtr t;
        if (!OpenProcessToken(p, 0x8, out t)) { CloseHandle(p); return "cannot open token (error " + Marshal.GetLastWin32Error() + ")"; }
        int len; GetTokenInformation(t, 25, IntPtr.Zero, 0, out len); // TokenIntegrityLevel
        IntPtr buf = Marshal.AllocHGlobal(len);
        string result = "unknown";
        if (GetTokenInformation(t, 25, buf, len, out len)) {
            IntPtr sid = Marshal.ReadIntPtr(buf);
            int count = Marshal.ReadByte(GetSidSubAuthorityCount(sid));
            int rid = Marshal.ReadInt32(GetSidSubAuthority(sid, (uint)(count - 1)));
            result = rid >= 0x4000 ? "System" : rid >= 0x3000 ? "High (admin)" : rid >= 0x2000 ? "Medium (normal)" : "Low";
            result += " 0x" + rid.ToString("x");
        }
        Marshal.FreeHGlobal(buf); CloseHandle(t); CloseHandle(p);
        return result;
    }
}
"@

$deadline = (Get-Date).AddSeconds($TimeoutSec)
while ((Get-Date) -lt $deadline) {
    $game = Get-Process | Where-Object { $_.Name -like "AnomalyDX11*" } | Select-Object -First 1
    if ($game) {
        $launcher = Get-Process AoeVrLauncher -ErrorAction SilentlyContinue | Select-Object -First 1
        "game     $($game.Name) pid $($game.Id): $([Integrity]::Get($game.Id))"
        if ($launcher) { "launcher pid $($launcher.Id): $([Integrity]::Get($launcher.Id))" }
        return
    }
    Start-Sleep -Milliseconds 500
}
"timeout: game did not start"
