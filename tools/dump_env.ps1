# Prints the environment block of a running 64-bit process (read from its PEB).
# Used to see what the Anomaly VR launcher passes to the game.
param([Parameter(Mandatory)][int]$ProcessId)

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class EnvDump {
    [StructLayout(LayoutKind.Sequential)] struct PBI { public IntPtr r1; public IntPtr Peb; public IntPtr r2; public IntPtr r3; public IntPtr Pid; public IntPtr r4; }
    [DllImport("ntdll.dll")] static extern int NtQueryInformationProcess(IntPtr h, int cls, ref PBI pbi, int len, out int ret);
    [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool ReadProcessMemory(IntPtr h, IntPtr addr, byte[] buf, IntPtr size, out IntPtr read);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")] static extern bool IsProcessInJob(IntPtr h, IntPtr job, out bool result);
    public static string Info(int pid) {
        IntPtr h = OpenProcess(0x0410, false, pid);
        if (h == IntPtr.Zero) return "OpenProcess failed: " + Marshal.GetLastWin32Error();
        var pbi = new PBI(); int ret;
        NtQueryInformationProcess(h, 0, ref pbi, Marshal.SizeOf(pbi), out ret);
        IntPtr parameters = ReadPtr(h, pbi.Peb + 0x20);
        // RTL_USER_PROCESS_PARAMETERS.CurrentDirectory: UNICODE_STRING at +0x38 (Length at +0x38, Buffer at +0x40)
        var lenBuf = new byte[2]; IntPtr r;
        ReadProcessMemory(h, parameters + 0x38, lenBuf, (IntPtr)2, out r);
        int len = BitConverter.ToUInt16(lenBuf, 0);
        IntPtr cwdPtr = ReadPtr(h, parameters + 0x40);
        var cwd = new byte[len];
        ReadProcessMemory(h, cwdPtr, cwd, (IntPtr)len, out r);
        bool inJob; IsProcessInJob(h, IntPtr.Zero, out inJob);
        CloseHandle(h);
        return "cwd=" + Encoding.Unicode.GetString(cwd) + "\njob=" + inJob;
    }
    static IntPtr ReadPtr(IntPtr h, IntPtr addr) { var b = new byte[8]; IntPtr r; ReadProcessMemory(h, addr, b, (IntPtr)8, out r); return (IntPtr)BitConverter.ToInt64(b, 0); }
    public static string Get(int pid) {
        IntPtr h = OpenProcess(0x0410, false, pid); // QUERY_INFORMATION | VM_READ
        if (h == IntPtr.Zero) return "OpenProcess failed: " + Marshal.GetLastWin32Error();
        var pbi = new PBI(); int ret;
        NtQueryInformationProcess(h, 0, ref pbi, Marshal.SizeOf(pbi), out ret);
        IntPtr parameters = ReadPtr(h, pbi.Peb + 0x20);
        IntPtr env = ReadPtr(h, parameters + 0x80);
        long size = (long)ReadPtr(h, parameters + 0x3F0);
        if (size <= 0 || size > 1 << 20) size = 64 * 1024;
        var buf = new byte[size]; IntPtr read;
        ReadProcessMemory(h, env, buf, (IntPtr)size, out read);
        CloseHandle(h);
        string s = Encoding.Unicode.GetString(buf, 0, (int)read);
        int end = s.IndexOf("\0\0");
        return (end >= 0 ? s.Substring(0, end) : s).Replace('\0', '\n');
    }
}
"@
[EnvDump]::Info($ProcessId)
[EnvDump]::Get($ProcessId)
