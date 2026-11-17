# Test helper for Geometry Wars 3: launch, send keys, screenshot, frame dump, quit.
#   gw.ps1 launch                 start through Steam and wait for the window
#   gw.ps1 keys "ENTER,500,DOWN"  press keys (numbers are waits in ms); only while the game is in front
#   gw.ps1 hold "W" 2000          hold one key for a time
#   gw.ps1 shot <png> [width]     DPI-aware screenshot of the game window's monitor area
#   gw.ps1 dump                   ask the proxy for a frame dump; prints the new file's name
#   gw.ps1 quit                   close the game
param([string]$cmd, [string]$a1, [string]$a2)
$game = 'H:\SteamLibrary\steamapps\common\Geometry Wars 3 - Dimensions'
Add-Type @'
using System; using System.Runtime.InteropServices; using System.Text;
public class GwNative {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    public struct RECT { public int L, T, R, B; }
    public struct POINT { public int X, Y; }
    public static string Title() { var sb = new StringBuilder(256); GetWindowText(GetForegroundWindow(), sb, 256); return sb.ToString(); }
    public static void Key(byte vk, bool up, bool ext) {
        byte sc = (byte)MapVirtualKey(vk, 0);
        keybd_event(vk, sc, (uint)((up ? 2 : 0) | (ext ? 1 : 0)), UIntPtr.Zero);
    }
}
'@
[GwNative]::SetProcessDPIAware() | Out-Null
$vk = @{ ENTER=0x0D; ESC=0x1B; SPACE=0x20; UP=0x26; DOWN=0x28; LEFT=0x25; RIGHT=0x27; TAB=0x09; BACK=0x08 }
function VkOf($k) { if ($vk.ContainsKey($k)) { $vk[$k] } else { [int][char]$k.ToUpper() } }
function IsExt($k) { @('UP','DOWN','LEFT','RIGHT') -contains $k }
function InFront { [GwNative]::Title() -like 'Geometry Wars 3*' }
function Proc { Get-Process GW3 -ErrorAction SilentlyContinue | Select-Object -First 1 }

switch ($cmd) {
    'launch' {
        if (-not (Proc)) { Start-Process 'steam://rungameid/310790' }
        for ($i = 0; $i -lt 40; $i++) { Start-Sleep -Milliseconds 500; $p = Proc; if ($p -and $p.MainWindowHandle -ne 0) { break } }
        $p = Proc; if (-not $p) { 'game did not start'; exit 1 }
        "pid $($p.Id) window '$($p.MainWindowTitle)'"
    }
    'front' {
        $p = Proc; if ($p) { [GwNative]::SetForegroundWindow($p.MainWindowHandle) | Out-Null; Start-Sleep -Milliseconds 300; "front: $(InFront)" }
    }
    'keys' {
        foreach ($k in $a1.Split(',')) {
            if ($k -match '^\d+$') { Start-Sleep -Milliseconds ([int]$k); continue }
            if (-not (InFront)) { "game not in front (front is '$([GwNative]::Title())'), stopped before $k"; exit 1 }
            $v = VkOf $k; $e = IsExt $k
            [GwNative]::Key($v, $false, $e); Start-Sleep -Milliseconds 80; [GwNative]::Key($v, $true, $e); Start-Sleep -Milliseconds 120
        }
        'keys sent'
    }
    'hold' {
        if (-not (InFront)) { "game not in front"; exit 1 }
        $v = VkOf $a1; $e = IsExt $a1
        try { [GwNative]::Key($v, $false, $e); Start-Sleep -Milliseconds ([int]$a2) } finally { [GwNative]::Key($v, $true, $e) }
        'held'
    }
    'shot' {
        Add-Type -AssemblyName System.Windows.Forms, System.Drawing
        $p = Proc; if (-not $p) { 'game not running'; exit 1 }
        $r = New-Object GwNative+RECT; [GwNative]::GetClientRect($p.MainWindowHandle, [ref]$r) | Out-Null
        $pt = New-Object GwNative+POINT; [GwNative]::ClientToScreen($p.MainWindowHandle, [ref]$pt) | Out-Null
        $bmp = New-Object System.Drawing.Bitmap $r.R, $r.B
        $g = [System.Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($pt.X, $pt.Y, 0, 0, $bmp.Size)
        $w = if ($a2) { [int]$a2 } else { 1280 }
        if ($w -lt $r.R) {
            $s = New-Object System.Drawing.Bitmap $w, ([int]($w * $r.B / $r.R))
            $g2 = [System.Drawing.Graphics]::FromImage($s); $g2.InterpolationMode = 'HighQualityBicubic'; $g2.DrawImage($bmp, 0, 0, $s.Width, $s.Height); $s.Save($a1)
        } else { $bmp.Save($a1) }
        "saved $a1 from $($r.R)x$($r.B) (in front: $(InFront))"
    }
    'dump' {
        $before = @(Get-ChildItem $game -Filter 'gw3vr_dump_*.log').Count
        New-Item -ItemType File "$game\gw3vr_dump.req" -Force | Out-Null
        for ($i = 0; $i -lt 20; $i++) { Start-Sleep -Milliseconds 200; if (@(Get-ChildItem $game -Filter 'gw3vr_dump_*.log').Count -gt $before -and -not (Test-Path "$game\gw3vr_dump.req")) { break } }
        Start-Sleep -Milliseconds 300
        (Get-ChildItem $game -Filter 'gw3vr_dump_*.log' | Sort-Object LastWriteTime | Select-Object -Last 1).Name
    }
    'quit' {
        $p = Proc; if ($p) { $p.CloseMainWindow() | Out-Null; Start-Sleep -Seconds 2; $p = Proc; if ($p) { Stop-Process -Id $p.Id -Force } }
        "running: $([bool](Proc))"
    }
}
