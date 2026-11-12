param(
    [Parameter(Mandatory)] [string]$Exe,       # the built music-visualizer.exe
    [Parameter(Mandatory)] [string]$Out,       # folder for screenshots
    [switch]$AllowInput,                       # also drive the real mouse (fine on a test machine, rude on your own PC)
    [switch]$SingleLaunch                      # skip the steps that start the app a second and third time
)
# End-to-end test of the app without sound: placement, lock, settings window, colour picker, tray menu,
# start with Windows, settings location, graphics rebuild and off-screen recovery.
# It changes this user's Music Visualizer settings and start-up entry, and restores neither. Meant for CI.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Text; using System.Runtime.InteropServices;
public class T {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern IntPtr GetWindowLongPtr(IntPtr h, int i);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, string l);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint f, UIntPtr e);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetTopWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
  public static bool IsBehind(IntPtr mine, IntPtr other) {   // true when "other" is in front of "mine"
    for (IntPtr w = GetTopWindow(IntPtr.Zero); w != IntPtr.Zero; w = GetWindow(w, 2)) {
      if (w == other) return true;
      if (w == mine) return false;
    }
    return false;
  }
  public static string ZOrder(IntPtr mine) {   // visible top-level windows from front to back
    var sb = new StringBuilder(); int n = 0;
    for (IntPtr w = GetTopWindow(IntPtr.Zero); w != IntPtr.Zero && n < 400; w = GetWindow(w, 2), n++) {
      if (!IsWindowVisible(w)) continue;
      var c = new StringBuilder(64); GetClassName(w, c, 64);
      sb.Append(w == mine ? "[WIDGET]" : c.ToString()).Append(" > ");
    }
    return sb.ToString();
  }
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  public static IntPtr Find(uint pid, string cls) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p == pid) {
      var c = new StringBuilder(64); GetClassName(h, c, 64); if (c.ToString() == cls && IsWindowVisible(h)) found = h; } return true; }, IntPtr.Zero);
    return found;
  }
}
'@
[T]::SetProcessDPIAware() | Out-Null
New-Item -ItemType Directory -Force $Out | Out-Null
$script:fails = 0
$appData = Join-Path $env:APPDATA 'MusicVisualizer'
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$zero = [IntPtr]::Zero

function Check($name, $ok, $detail) {
    if (-not $ok) { $script:fails++ }
    "{0}  {1}  {2}" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $name, $detail
}
function Skip($name, $why) { "SKIP  $name  $why" }
function WinRect($w) { $q = New-Object T+RECT; [T]::GetWindowRect($w, [ref]$q) | Out-Null; $q }
function ExStyle($w) { [T]::GetWindowLongPtr($w, -20).ToInt64() }
function Grab($x, $y, $w, $ht) {
    $bmp = New-Object System.Drawing.Bitmap $w, $ht
    $g = [System.Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($x, $y, 0, 0, $bmp.Size); $g.Dispose(); $bmp
}
function ShotScreen($name) {
    $bmp = Grab 0 0 ([T]::GetSystemMetrics(0)) ([T]::GetSystemMetrics(1))
    $bmp.Save((Join-Path $Out "$name.png")); $bmp.Dispose()
}
function ShotWindow($w, $name) {
    $q = WinRect $w
    $bmp = Grab $q.L $q.T ($q.R - $q.L) ($q.B - $q.T)
    $bmp.Save((Join-Path $Out "$name.png")); $bmp.Dispose()
}
# How many pixels inside the widget are tinted with the given (saturated) colour. Sampled every 2 px.
function CountColour($w, $cr, $cg, $cb) {
    $q = WinRect $w; $bmp = Grab $q.L $q.T ($q.R - $q.L) ($q.B - $q.T); $n = 0
    $tlo = [Math]::Min($cr, [Math]::Min($cg, $cb)); $tspan = [double]([Math]::Max($cr, [Math]::Max($cg, $cb)) - $tlo)
    for ($y = 0; $y -lt $bmp.Height; $y += 2) { for ($x = 0; $x -lt $bmp.Width; $x += 2) {
        $c = $bmp.GetPixel($x, $y)
        # The bars are see-through (brightest at the top, fading to the foot), so a pixel is the colour mixed
        # with whatever is behind it. Count it when it is clearly coloured and its hue matches.
        $lo = [Math]::Min($c.R, [Math]::Min($c.G, $c.B)); $span = [Math]::Max($c.R, [Math]::Max($c.G, $c.B)) - $lo
        if ($span -ge 60 -and [Math]::Abs(($c.R - $lo) / $span - ($cr - $tlo) / $tspan) -lt 0.3 -and
            [Math]::Abs(($c.G - $lo) / $span - ($cg - $tlo) / $tspan) -lt 0.3 -and
            [Math]::Abs(($c.B - $lo) / $span - ($cb - $tlo) / $tspan) -lt 0.3) { $n++ }
    } }
    $bmp.Dispose(); $n
}
function IniPathNow { if (Test-Path (Join-Path (Split-Path $script:exeNow) 'settings.ini')) { Join-Path (Split-Path $script:exeNow) 'settings.ini' } else { Join-Path $appData 'settings.ini' } }
function IniGet($k) { $line = Get-Content (IniPathNow) | Where-Object { $_ -match "^$k\s*=" } | Select-Object -First 1; if ($line) { ($line -split '=', 2)[1].Trim() } else { $null } }
function IniSet($pairs) {
    $path = IniPathNow
    $lines = [System.Collections.Generic.List[string]](Get-Content $path)
    foreach ($k in $pairs.Keys) {
        $i = -1; for ($n = 0; $n -lt $lines.Count; $n++) { if ($lines[$n] -match "^$k\s*=") { $i = $n } }
        if ($i -ge 0) { $lines[$i] = "$k=$($pairs[$k])" } else { $lines.Add("$k=$($pairs[$k])") }
    }
    Set-Content -Path $path -Value $lines -Encoding ASCII
}
function MakeLP($x, $y) { [IntPtr]((($y -band 0xFFFF) -shl 16) -bor ($x -band 0xFFFF)) }
function HitTest($w, $x, $y) { [T]::SendMessage($w, 0x84, $zero, (MakeLP $x $y)).ToInt64() }
function WaitFor($what, $secs, [scriptblock]$test) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt $secs) { if (& $test) { return $true }; Start-Sleep -Milliseconds 150 }
    $false
}
function StartApp($path) {
    $script:exeNow = $path
    $script:app = Start-Process $path -PassThru
    $ok = WaitFor 'widget' 8 { [T]::Find([uint32]$script:app.Id, 'MusicVisualizer') -ne $zero }
    $script:wid = [T]::Find([uint32]$script:app.Id, 'MusicVisualizer')
    $ok
}
function StopApp {
    if ($script:app -and -not $script:app.HasExited) {
        [T]::PostMessage($script:wid, 0x10, $zero, $zero) | Out-Null   # WM_CLOSE
        if (-not $script:app.WaitForExit(4000)) { $script:app.Kill() }
    }
}
function OpenMenu { [T]::PostMessage($script:wid, 0x8001, $zero, [IntPtr]0x205) | Out-Null; WaitFor 'menu' 3 { [T]::Find([uint32]$script:app.Id, '#32768') -ne $zero } }
function MenuKeys($keys) {   # keys go to the open menu
    $m = [T]::Find([uint32]$script:app.Id, '#32768')
    foreach ($k in $keys) { [T]::PostMessage($m, 0x100, [IntPtr]$k, $zero) | Out-Null; Start-Sleep -Milliseconds 120 }
}

# ---------------------------------------------------------------- clean slate
Get-Process music-visualizer -ErrorAction SilentlyContinue | Stop-Process -Force
Remove-Item $appData -Recurse -Force -ErrorAction SilentlyContinue
Remove-ItemProperty -Path $runKey -Name MusicVisualizer -ErrorAction SilentlyContinue
$work = Join-Path $env:TEMP ("mvui_" + [Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $work | Out-Null
$exe1 = Join-Path $work 'music-visualizer.exe'
Copy-Item $Exe $exe1

# ---------------------------------------------------------------- 1. first run
Check 'app starts and shows its widget' (StartApp $exe1) ''
if ($script:wid -eq $zero) { ShotScreen 'failed-start'; "cannot continue"; exit 1 }
Start-Sleep -Seconds 2
$wid = $script:wid
Check 'settings are created in AppData' (Test-Path (Join-Path $appData 'settings.ini')) (Join-Path $appData 'settings.ini')
Check 'nothing is written next to the exe' (-not (Test-Path (Join-Path $work 'settings.ini'))) ''
$q = WinRect $wid
$sw0 = [T]::GetSystemMetrics(0); $sh0 = [T]::GetSystemMetrics(1)
Check 'starts unlocked and on top' ((((ExStyle $wid) -band 0x8) -ne 0) -and (((ExStyle $wid) -band 0x20) -eq 0)) ("ex={0:X}" -f (ExStyle $wid))
Check 'starts fully on the screen' ($q.L -ge 0 -and $q.T -ge 0 -and $q.R -le $sw0 -and $q.B -le $sh0 -and ($q.R - $q.L) -ge 240) "rect $($q.L),$($q.T) $($q.R - $q.L)x$($q.B - $q.T) on ${sw0}x${sh0}"
IniSet @{ color = '00F0FF' }   # the default colour is a soft white; the pixel checks need a saturated one
Start-Sleep -Seconds 3
ShotScreen '1-first-run'
$cyan = CountColour $wid 0 240 255
Check 'the widget is actually drawn' ($cyan -gt 150) "$cyan cyan pixels sampled"

# ---------------------------------------------------------------- 2. handles
$cx = [int](($q.L + $q.R) / 2); $cy = [int](($q.T + $q.B) / 2)
Check 'drag handle in the middle' ((HitTest $wid $cx $cy) -eq 2) "hit=$(HitTest $wid $cx $cy)"
Check 'resize handle on the left edge' ((HitTest $wid ($q.L + 2) $cy) -eq 10) ''
Check 'resize handle on the top edge' ((HitTest $wid $cx ($q.T + 2)) -eq 12) ''
Check 'resize handle in the bottom-right corner' ((HitTest $wid ($q.R - 2) ($q.B - 2)) -eq 17) ''

# ---------------------------------------------------------------- 3. real mouse drag
$canInput = $false
if ($AllowInput) {
    [T]::SetCursorPos($cx, $cy) | Out-Null; Start-Sleep -Milliseconds 200
    $pt = New-Object T+POINT; [T]::GetCursorPos([ref]$pt) | Out-Null
    $canInput = ([Math]::Abs($pt.X - $cx) -le 2 -and [Math]::Abs($pt.Y - $cy) -le 2)
}
if ($canInput) {
    [T]::mouse_event(0x2, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 150
    for ($i = 1; $i -le 10; $i++) { [T]::SetCursorPos($cx - 6 * $i, $cy - 4 * $i) | Out-Null; [T]::mouse_event(0x1, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 40 }
    [T]::mouse_event(0x4, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Seconds 2
    $m = WinRect $wid
    Check 'dragging with the mouse moves it' ([Math]::Abs(($m.L - $q.L) + 60) -le 6 -and [Math]::Abs(($m.T - $q.T) + 40) -le 6) "moved by $($m.L - $q.L),$($m.T - $q.T), expected -60,-40"
    Check 'the dragged position is saved' ((IniGet 'x') -eq "$($m.L)" -and (IniGet 'y') -eq "$($m.T)") "ini x=$(IniGet 'x') y=$(IniGet 'y')"
} else {
    Skip 'dragging with the mouse' 'real mouse input not allowed or not available; moving the window directly instead'
    [T]::SetWindowPos($wid, $zero, $q.L - 60, $q.T - 40, 0, 0, 0x15) | Out-Null
    [T]::PostMessage($wid, 0x232, $zero, $zero) | Out-Null; Start-Sleep -Seconds 2
    Check 'a moved position is saved' ((IniGet 'x') -eq "$($q.L - 60)" -and (IniGet 'y') -eq "$($q.T - 40)") "ini x=$(IniGet 'x') y=$(IniGet 'y')"
}
$moved = WinRect $wid

# ---------------------------------------------------------------- 4. lock by double-click
$mx = [int](($moved.L + $moved.R) / 2); $my = [int](($moved.T + $moved.B) / 2)
[T]::PostMessage($wid, 0xA3, [IntPtr]2, (MakeLP $mx $my)) | Out-Null
Start-Sleep -Seconds 2
Check 'double-click locks and saves it' ((IniGet 'locked') -eq '1') "ini locked=$(IniGet 'locked')"
Check 'locked: clicks pass through' ((((ExStyle $wid) -band 0x20) -ne 0) -and ((HitTest $wid $mx $my) -eq -1)) ("ex={0:X}" -f (ExStyle $wid))
Check 'locked: no longer on top' (((ExStyle $wid) -band 0x8) -eq 0) ''

# ---------------------------------------------------------------- 5. tray menu
if (OpenMenu) {
    Check 'tray menu opens' $true ''
    MenuKeys @(0x28, 0x0D)          # Down (Lock position), Enter
    Start-Sleep -Seconds 2
    Check 'menu: Lock position unlocks it again' ((IniGet 'locked') -eq '0' -and (((ExStyle $wid) -band 0x8) -ne 0)) "ini locked=$(IniGet 'locked')"
    if (OpenMenu) {
        MenuKeys @(0x28, 0x28, 0x28, 0x0D)   # third item: Start with Windows
        Start-Sleep -Seconds 1
        $run = (Get-ItemProperty -Path $runKey -ErrorAction SilentlyContinue).MusicVisualizer
        Check 'menu: Start with Windows adds the start-up entry' ($run -eq "`"$exe1`"") "entry: $run"
    } else { Check 'tray menu opens a second time' $false '' }
} else { Check 'tray menu opens' $false '' }

# ---------------------------------------------------------------- 6. settings window
[T]::PostMessage($wid, 0x8002, $zero, $zero) | Out-Null
$opened = WaitFor 'settings' 4 { [T]::Find([uint32]$script:app.Id, 'MusicVisualizerSettings') -ne $zero }
Check 'settings window opens' $opened ''
$set = [T]::Find([uint32]$script:app.Id, 'MusicVisualizerSettings')
if ($opened) {
    [T]::SetWindowPos($set, [IntPtr](-1), 40, 40, 0, 0, 0x11) | Out-Null; Start-Sleep -Milliseconds 700
    ShotWindow $set '2-settings'
    # control ids: 10 colour button, 13 columns, 14 frames/sec, 15 hide on fullscreen, 18 reset, 19 close, 20 start with Windows
    Check 'settings: Start with Windows box shows the entry' ([T]::SendMessage([T]::GetDlgItem($set, 20), 0xF0, $zero, $zero).ToInt64() -eq 1) ''
    [T]::SendMessage([T]::GetDlgItem($set, 20), 0xF5, $zero, $zero) | Out-Null; Start-Sleep -Milliseconds 600
    $run = (Get-ItemProperty -Path $runKey -ErrorAction SilentlyContinue).MusicVisualizer
    Check 'settings: unticking removes the start-up entry' ($null -eq $run) "entry: $run"

    $sl = [T]::GetDlgItem($set, 13)
    [T]::SendMessage($sl, 0x405, [IntPtr]1, [IntPtr]24) | Out-Null; [T]::SendMessage($set, 0x114, [IntPtr]8, $sl) | Out-Null
    $sl = [T]::GetDlgItem($set, 14)
    [T]::SendMessage($sl, 0x405, [IntPtr]1, [IntPtr]30) | Out-Null; [T]::SendMessage($set, 0x114, [IntPtr]8, $sl) | Out-Null
    [T]::SendMessage([T]::GetDlgItem($set, 15), 0xF5, $zero, $zero) | Out-Null    # hide on fullscreen off
    Start-Sleep -Seconds 1
    Check 'settings: sliders save' ((IniGet 'columns') -eq '24' -and (IniGet 'fps') -eq '30') "columns=$(IniGet 'columns') fps=$(IniGet 'fps')"
    Check 'settings: checkboxes save' ((IniGet 'hide_on_fullscreen') -eq '0') "hide_on_fullscreen=$(IniGet 'hide_on_fullscreen')"

    # colour picker: type a colour into the standard Windows dialog and accept it
    [T]::PostMessage($set, 0x111, [IntPtr]10, $zero) | Out-Null
    $dlgOpen = WaitFor 'colour dialog' 4 { [T]::Find([uint32]$script:app.Id, '#32770') -ne $zero }
    Check 'colour picker opens' $dlgOpen ''
    if ($dlgOpen) {
        $dlg = [T]::Find([uint32]$script:app.Id, '#32770')
        ShotWindow $dlg '3-colour-picker'
        [T]::SendMessage([T]::GetDlgItem($dlg, 706), 0x0C, $zero, '255') | Out-Null
        [T]::SendMessage([T]::GetDlgItem($dlg, 707), 0x0C, $zero, '0') | Out-Null
        [T]::SendMessage([T]::GetDlgItem($dlg, 708), 0x0C, $zero, '128') | Out-Null
        Start-Sleep -Milliseconds 400
        [T]::PostMessage($dlg, 0x111, [IntPtr]1, $zero) | Out-Null      # OK
        Start-Sleep -Seconds 2
        Check 'colour picker: chosen colour is saved' ((IniGet 'color') -eq 'FF0080') "ini color=$(IniGet 'color')"
        $pink = CountColour $wid 255 0 128
        Check 'colour picker: the widget changes colour' ($pink -gt 150) "$pink pixels in the new colour"
    }
    # Reset position puts it back at the default spot
    [T]::SendMessage([T]::GetDlgItem($set, 18), 0xF5, $zero, $zero) | Out-Null; Start-Sleep -Seconds 2
    $rr = WinRect $wid
    Check 'settings: Reset position' ($rr.L -eq $q.L -and $rr.T -eq $q.T) "rect $($rr.L),$($rr.T), default was $($q.L),$($q.T)"
    ShotScreen '4-after-settings'
    [T]::SendMessage([T]::GetDlgItem($set, 19), 0xF5, $zero, $zero) | Out-Null
    Check 'settings window closes' (WaitFor 'closed' 3 { [T]::Find([uint32]$script:app.Id, 'MusicVisualizerSettings') -eq $zero }) ''
}

# ---------------------------------------------------------------- 7. editing the file applies live
IniSet @{ color = '39FF14' }
Check 'editing settings.ini applies within 3 s' (WaitFor 'green' 3 { (CountColour $wid 57 255 20) -gt 150 }) ''

# ---------------------------------------------------------------- 8. graphics device lost
[T]::PostMessage($wid, 0x8003, $zero, $zero) | Out-Null
Start-Sleep -Seconds 3
IniSet @{ color = '00F0FF' }
Check 'recovers after losing the graphics device' ((-not $script:app.HasExited) -and (WaitFor 'cyan' 4 { (CountColour $wid 0 240 255) -gt 150 })) ''

# ---------------------------------------------------------------- 9. saved spot is off every screen
IniSet @{ x = '-20000'; y = '-20000' }
$back = WaitFor 'on screen' 4 { $z = WinRect $wid; $z.L -ge 0 -and $z.T -ge 0 -and $z.R -le $sw0 -and $z.B -le $sh0 }
$z = WinRect $wid
Check 'comes back when its saved spot is off-screen' $back "rect $($z.L),$($z.T)"

# ---------------------------------------------------------------- 9b. locked, with (test) music
function CpuPercent($secs) {   # share of one core used by the app over a few seconds
    $script:app.Refresh(); $c0 = $script:app.TotalProcessorTime.TotalSeconds; $sw = [Diagnostics.Stopwatch]::StartNew()
    Start-Sleep -Seconds $secs
    $script:app.Refresh(); ($script:app.TotalProcessorTime.TotalSeconds - $c0) / $sw.Elapsed.TotalSeconds * 100
}
IniSet @{ locked = '1'; always_on_top = '1'; opacity = '100'; columns = '32'; fps = '60' }
Start-Sleep -Seconds 3
Check 'locked and silent: nothing is drawn' ((CountColour $wid 0 240 255) -lt 30) "$(CountColour $wid 0 240 255) pixels"
$idleA = CpuPercent 3; $idleB = CpuPercent 3     # two samples: asleep is a steady state, a one-off blip is not a leak
Check 'asleep: uses almost no CPU' ([Math]::Min($idleA, $idleB) -lt 1.0) ("{0:N2}% and {1:N2}% of one core" -f $idleA, $idleB)
[T]::PostMessage($wid, 0x8004, [IntPtr]1, $zero) | Out-Null       # built-in test signal on
$lit = WaitFor 'bars' 4 { (CountColour $wid 0 240 255) -gt 500 }
Check 'locked: wakes up and draws the music' $lit "$(CountColour $wid 0 240 255) lit pixels"
Start-Sleep -Seconds 6
ShotWindow $wid '6-locked-playing'
ShotScreen '6-locked-playing-screen'
"INFO  CPU while drawing on this machine (software graphics): {0:N1}% of one core" -f (CpuPercent 4)
if ($AllowInput) {   # the widget living on the desktop, behind other windows, and Win+D ("Show desktop")
    IniSet @{ always_on_top = '0' }
    Start-Sleep -Seconds 3
    $front = [T]::GetForegroundWindow()
    "INFO  window order, front to back: $([T]::ZOrder($wid))"
    [T]::ShowWindow($front, 6) | Out-Null          # minimise the window in front (the test console)
    Start-Sleep -Seconds 2
    ShotScreen '7-on-desktop'
    Check 'on the desktop: visible when nothing covers it' (WaitFor 'lit' 4 { (CountColour $wid 0 240 255) -gt 500 }) ''
    [T]::ShowWindow($front, 9) | Out-Null          # bring it back
    Start-Sleep -Seconds 2
    Check 'on the desktop: stays behind other windows' ([T]::IsBehind($wid, $front)) "window order: $([T]::ZOrder($wid))"
    [T]::keybd_event(0x5B, 0, 0, [UIntPtr]::Zero); [T]::keybd_event(0x44, 0, 0, [UIntPtr]::Zero)
    [T]::keybd_event(0x44, 0, 2, [UIntPtr]::Zero); [T]::keybd_event(0x5B, 0, 2, [UIntPtr]::Zero)
    Start-Sleep -Seconds 3
    ShotScreen '8-show-desktop'
    "INFO  after Win+D, front to back: $([T]::ZOrder($wid))"
    Check 'on the desktop: still visible after Win+D' (WaitFor 'lit' 4 { (CountColour $wid 0 240 255) -gt 500 }) "minimised=$([T]::IsIconic($wid))"
    [T]::keybd_event(0x5B, 0, 0, [UIntPtr]::Zero); [T]::keybd_event(0x44, 0, 0, [UIntPtr]::Zero)
    [T]::keybd_event(0x44, 0, 2, [UIntPtr]::Zero); [T]::keybd_event(0x5B, 0, 2, [UIntPtr]::Zero)
    Start-Sleep -Seconds 3
    Check 'on the desktop: goes back behind windows after Win+D' ([T]::IsBehind($wid, $front)) "window order: $([T]::ZOrder($wid))"
    IniSet @{ always_on_top = '1' }
    Start-Sleep -Seconds 2
}
[T]::PostMessage($wid, 0x8004, $zero, $zero) | Out-Null           # signal off
Check 'fades out when the music stops' (WaitFor 'dark' 6 { (CountColour $wid 0 240 255) -lt 30 }) "$(CountColour $wid 0 240 255) pixels"
IniSet @{ locked = '0'; always_on_top = '0' }
Start-Sleep -Seconds 3

# ---------------------------------------------------------------- 10. second launch opens settings
if (-not $SingleLaunch) {
    $second = Start-Process $exe1 -PassThru
    Check 'second launch exits by itself' ($second.WaitForExit(5000)) ''
    Check 'second launch opens the settings of the first' (WaitFor 'settings' 4 { [T]::Find([uint32]$script:app.Id, 'MusicVisualizerSettings') -ne $zero }) ''
    Check 'still one copy running' (@(Get-Process music-visualizer).Count -eq 1) ''
}

# ---------------------------------------------------------------- 11. quit from the menu
if (OpenMenu) {
    MenuKeys @(0x26, 0x0D)          # Up wraps to the last item (Quit), Enter
    Check 'menu: Quit ends the app' ($script:app.WaitForExit(5000)) ''
} else { Check 'tray menu opens for Quit' $false ''; StopApp }

# ---------------------------------------------------------------- 12. portable mode
if (-not $SingleLaunch) {
    $stamp = (Get-Item (Join-Path $appData 'settings.ini')).LastWriteTimeUtc
    $port = Join-Path $work 'portable'; New-Item -ItemType Directory -Force $port | Out-Null
    $exe2 = Join-Path $port 'music-visualizer.exe'; Copy-Item $Exe $exe2
    Set-Content -Path (Join-Path $port 'settings.ini') -Value '[visualizer]', 'color=FF2A6D', 'opacity=100', 'locked=0' -Encoding ASCII
    Check 'portable copy starts' (StartApp $exe2) ''
    Start-Sleep -Seconds 3
    $wid = $script:wid
    Check 'portable: uses the settings.ini next to the exe' ((CountColour $wid 255 42 109) -gt 150) ''
    [T]::PostMessage($wid, 0xA3, [IntPtr]2, $zero) | Out-Null; Start-Sleep -Seconds 2
    Check 'portable: saves next to the exe' ((Select-String -Path (Join-Path $port 'settings.ini') -Pattern '^locked=1' -Quiet)) ''
    Check 'portable: leaves the AppData settings alone' ((Get-Item (Join-Path $appData 'settings.ini')).LastWriteTimeUtc -eq $stamp) ''
    ShotScreen '5-portable'
    StopApp
    Check 'closes cleanly' ($script:app.HasExited) ''
}

"`n$($script:fails) failed"
exit $(if ($script:fails) { 1 } else { 0 })
