# Types a console command into the running game: focuses its window, switches it to the
# US keyboard layout, opens the console (~), types the command as hardware scan codes
# (the engine reads the keyboard through DirectInput) and presses Enter.
#
# usage: game_console.ps1 -Command "load save_2"
param([Parameter(Mandatory)][string]$Command, [switch]$NoConsoleToggle)

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class GameInput {
    [StructLayout(LayoutKind.Sequential)] struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Explicit, Size = 40)] struct INPUT { [FieldOffset(0)] public uint type; [FieldOffset(8)] public KEYBDINPUT ki; }
    [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] inputs, int size);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern IntPtr LoadKeyboardLayout(string id, uint flags);
    [DllImport("user32.dll")] static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);

    const uint KEYEVENTF_KEYUP = 0x2, KEYEVENTF_SCANCODE = 0x8;

    public static void Scan(ushort scan, bool up) {
        var i = new INPUT[1];
        i[0].type = 1;
        i[0].ki.wScan = scan;
        i[0].ki.dwFlags = KEYEVENTF_SCANCODE | (up ? KEYEVENTF_KEYUP : 0);
        SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
    }
    public static void Tap(ushort scan) { Scan(scan, false); System.Threading.Thread.Sleep(30); Scan(scan, true); System.Threading.Thread.Sleep(30); }
    public static void ShiftTap(ushort scan) { Scan(0x2A, false); System.Threading.Thread.Sleep(20); Tap(scan); Scan(0x2A, true); System.Threading.Thread.Sleep(20); }

    // Windows only lets the foreground app hand over focus; a synthetic ALT press counts as user input
    public static bool Focus(IntPtr h) {
        keybd_event(0x12, 0, 0, UIntPtr.Zero);
        keybd_event(0x12, 0, KEYEVENTF_KEYUP, UIntPtr.Zero);
        ShowWindow(h, 9);
        SetForegroundWindow(h);
        return GetForegroundWindow() == h;
    }
}
"@

# US layout scan codes
$scan = @{
    'a'=0x1E;'b'=0x30;'c'=0x2E;'d'=0x20;'e'=0x12;'f'=0x21;'g'=0x22;'h'=0x23;'i'=0x17;'j'=0x24;'k'=0x25;'l'=0x26;'m'=0x32
    'n'=0x31;'o'=0x18;'p'=0x19;'q'=0x10;'r'=0x13;'s'=0x1F;'t'=0x14;'u'=0x16;'v'=0x2F;'w'=0x11;'x'=0x2D;'y'=0x15;'z'=0x2C
    '1'=0x02;'2'=0x03;'3'=0x04;'4'=0x05;'5'=0x06;'6'=0x07;'7'=0x08;'8'=0x09;'9'=0x0A;'0'=0x0B
    ' '=0x39;'-'=0x0C;'.'=0x34;'/'=0x35
}
$shifted = @{ '_'=0x0C }

$game = Get-Process AnomalyDX11 -ErrorAction Stop | Select-Object -First 1
$hwnd = $game.MainWindowHandle
if ($hwnd -eq [IntPtr]::Zero) { throw "game window not found" }

# WM_INPUTLANGCHANGEREQUEST with the US layout
$us = [GameInput]::LoadKeyboardLayout("00000409", 1)
[GameInput]::PostMessage($hwnd, 0x50, [IntPtr]::Zero, $us) | Out-Null
if (-not [GameInput]::Focus($hwnd)) { Start-Sleep -Milliseconds 300; [GameInput]::Focus($hwnd) | Out-Null }
Start-Sleep -Milliseconds 500
"foreground is game: $([GameInput]::GetForegroundWindow() -eq $hwnd)"

if (-not $NoConsoleToggle) { [GameInput]::Tap(0x29); Start-Sleep -Milliseconds 400 } # ~ opens the console
foreach ($ch in $Command.ToLower().ToCharArray()) {
    $c = [string]$ch
    if ($scan.ContainsKey($c)) { [GameInput]::Tap([uint16]$scan[$c]) }
    elseif ($shifted.ContainsKey($c)) { [GameInput]::ShiftTap([uint16]$shifted[$c]) }
    else { throw "no scan code for '$c'" }
}
Start-Sleep -Milliseconds 200
[GameInput]::Tap(0x1C) # Enter
"sent: $Command"
