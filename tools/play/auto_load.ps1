# Drives a freshly launched game to the loaded save without the user:
# presses START (via the PAD_FILE the game was launched with) until the
# save-data list dialog appears, then confirms it with Enter.
# Launch the game with: --env PAD_FILE=C:\GameDev\Macross\native\play\pad.txt
param([int]$TimeoutSec = 180)
$ErrorActionPreference = 'Stop'
$play = 'C:\GameDev\Macross\native\play'
$pad = Join-Path $play 'pad.txt'
Add-Type @'
using System; using System.Runtime.InteropServices;
public class AL {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
}
'@
$loadsBefore = (Select-String -Path (Join-Path $play 'stdout.log') -Pattern 'LOAD complete' -SimpleMatch -ErrorAction SilentlyContinue | Measure-Object).Count
$t0 = Get-Date
$seenDialog = $false
while (((Get-Date) - $t0).TotalSeconds -lt $TimeoutSec) {
    if (-not (Get-Process Macross30Recompiled -ErrorAction SilentlyContinue)) { 'game exited'; exit 1 }
    $loads = (Select-String -Path (Join-Path $play 'stdout.log') -Pattern 'LOAD complete' -SimpleMatch -ErrorAction SilentlyContinue | Measure-Object).Count
    if ($loads -gt $loadsBefore) { "loaded after {0:N0}s" -f ((Get-Date) - $t0).TotalSeconds; exit 0 }
    $dlg = [AL]::FindWindow('Ps3SaveDataListDlg', [NullString]::Value)
    if ($dlg -ne [IntPtr]::Zero) {
        $seenDialog = $true
        Start-Sleep -Milliseconds 500
        [AL]::PostMessage($dlg, 0x0100, [IntPtr]0x0D, [IntPtr]0) | Out-Null   # WM_KEYDOWN VK_RETURN
        Start-Sleep -Seconds 2
        continue
    }
    # After the dialog was answered, stop pressing START: one more press
    # lands in gameplay and opens the pause menu.
    if (-not $seenDialog) { Set-Content -Path $pad -Value '0x0008 6' -NoNewline }   # START, held 6 polls
    Start-Sleep -Milliseconds 1500
}
'timeout'; exit 2
