# Unattended Xenia run: boots dist\butterscotch (optionally straight into one
# data.win via BOOT.TXT), keeps the window on the secondary monitor, presses the
# given keys, screenshots the window and keeps Xenia's log.
#
#   .\tools\xdk\xenia_test.ps1 -Boot "chapter5_windows\data.win" -Seconds 40 -Name ch5
#   -Keys "5:{ENTER},8:z"   seconds-from-start:SendKeys-text pairs (Xenia maps the keyboard to the pad)
param(
    [string]$Boot = "",
    [int]$Seconds = 40,
    [string]$Name = "run",
    [string]$Keys = "",
    [string]$Xenia = "E:\downloads\xenia_canary_windows\xenia_canary.exe",
    [string]$Game = "$PSScriptRoot\..\..\dist\butterscotch"
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class W {
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  public struct RECT { public int L, T, R, B; }
}
"@
$out = Join-Path $PSScriptRoot "..\..\build-xbox360-xdk\tests"
New-Item -ItemType Directory -Force $out | Out-Null
$bootFile = Join-Path $Game "BOOT.TXT"
if ($Boot) { Set-Content -Encoding ascii $bootFile $Boot } elseif (Test-Path $bootFile) { Remove-Item $bootFile }

Get-Process xenia_canary -ErrorAction SilentlyContinue | Stop-Process -Force
$p = Start-Process $Xenia -ArgumentList "`"$(Resolve-Path $Game)\default.xex`"" -WorkingDirectory (Split-Path $Xenia) -PassThru
$screen = ([System.Windows.Forms.Screen]::AllScreens | Where-Object { -not $_.Primary } | Select-Object -First 1)
if (-not $screen) { $screen = [System.Windows.Forms.Screen]::PrimaryScreen }
$b = $screen.WorkingArea
for ($i = 0; $i -lt 50 -and $p.MainWindowHandle -eq 0; $i++) { Start-Sleep -Milliseconds 200; $p.Refresh() }
[W]::SetWindowPos($p.MainWindowHandle, [IntPtr]::Zero, $b.X + 20, $b.Y + 20, 1300, 780, 0x0040) | Out-Null

$plan = @{}
foreach ($k in ($Keys -split "," | Where-Object { $_ })) { $t, $s = $k -split ":", 2; $plan[[int]$t] = $s }
for ($t = 1; $t -le $Seconds; $t++) {
    Start-Sleep -Seconds 1
    if ($p.HasExited) { break }
    if ($plan.ContainsKey($t)) {
        [W]::SetForegroundWindow($p.MainWindowHandle) | Out-Null  # SendKeys goes to the focused window
        [System.Windows.Forms.SendKeys]::SendWait($plan[$t])
    }
}
if (-not $p.HasExited) {
    $r = New-Object W+RECT
    [W]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
    $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
    [System.Drawing.Graphics]::FromImage($bmp).CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
    $bmp.Save((Join-Path $out "$Name.png"))
    $p | Stop-Process -Force
} else { "Xenia exited early (code $($p.ExitCode))" }
Start-Sleep -Milliseconds 500
Copy-Item (Join-Path (Split-Path $Xenia) "xenia.log") (Join-Path $out "$Name.log") -Force
if ($Boot) { Remove-Item $bootFile }
$log = Get-Content (Join-Path $out "$Name.log")
"--- $Name : runner lines ---"
$log | Select-String "BS: BOOT|texture cache|no writable|FATAL|CRASH|Access Violation|no room|could not decode|disassemble|Unknown function|not implemented" | Select-Object -First 25 | ForEach-Object { $_.Line }
$log | Select-String "BS perf" | Select-Object -Last 2 | ForEach-Object { $_.Line }
