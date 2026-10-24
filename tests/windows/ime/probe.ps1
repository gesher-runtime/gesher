# Temporary CI probe (ime track), round 2: a real IME in a window of another
# thread, chosen from outside (as a test of a Gesher window must), the
# candidate window's position, the touch keyboard.
$ErrorActionPreference = 'Continue'
Write-Host "OS: $((Get-CimInstance Win32_OperatingSystem).Caption)"
$langs = { powershell.exe -NoProfile -Command 'Get-WinUserLanguageList | ForEach-Object { "language " + $_.LanguageTag + ": " + ($_.InputMethodTips -join ", ") }' }
& $langs
Add-Type -Path "$PSScriptRoot/probe.cs"
# Enable the Japanese IME's profile from this (the test's) thread, then start the window.
Write-Host ("ActivateHere ja: 0x{0:x8}" -f [ImeProbe]::ActivateHere('ja'))
& $langs
[ImeProbe]::Start()
Start-Sleep 1
Write-Host "foreground: $([ImeProbe]::Foreground([ImeProbe]::Hwnd))"
Write-Host "layouts: $([ImeProbe]::Layouts())"
function Step($name, [scriptblock] $s, [int] $ms = 1500) {
  & $s
  Start-Sleep -Milliseconds $ms
  Write-Host "--- $name"
  Write-Host (([ImeProbe]::Log() -split "`n" | Where-Object { $_ -notmatch 'WM_IME_NOTIFY' }) -join "`n")
}
Step 'ja layout request from outside' {
  [void][ImeProbe]::PostMessageW([ImeProbe]::Hwnd, 0x50, [IntPtr]0, [ImeProbe]::LoadKeyboardLayoutW('00000411', 0))
}
Step 'state' { [void][ImeProbe]::PostMessageW([ImeProbe]::Hwnd, 0x401, [IntPtr]0, [IntPtr]0) }
Step 'caret and IME windows' { [void][ImeProbe]::PostMessageW([ImeProbe]::Hwnd, 0x405, [IntPtr]0, [IntPtr]0) }
Write-Host ([ImeProbe]::Caret())
Step 'ja keys n i h o n (IME off: as is)' { [ImeProbe]::Keys(0x4E, 0x49, 0x48, 0x4F, 0x4E) | Out-Null }
Step 'ja Kanji key (VK_KANJI 0x19) turns the IME on' { [ImeProbe]::Keys(0x19) | Out-Null }
Step 'state' { [void][ImeProbe]::PostMessageW([ImeProbe]::Hwnd, 0x401, [IntPtr]0, [IntPtr]0) }
Step 'ja keys n i h o n' { [ImeProbe]::Keys(0x4E, 0x49, 0x48, 0x4F, 0x4E) | Out-Null }
Step 'space (convert)' { [ImeProbe]::Keys(0x20) | Out-Null }
Step 'space (candidates)' { [ImeProbe]::Keys(0x20) | Out-Null } 2500
Write-Host ([ImeProbe]::Windows())
Step 'enter' { [ImeProbe]::Keys(0x0D) | Out-Null }
Step 'ko layout request from outside' {
  Write-Host ("ActivateHere ko: 0x{0:x8}" -f [ImeProbe]::ActivateHere('ko'))
  [void][ImeProbe]::PostMessageW([ImeProbe]::Hwnd, 0x50, [IntPtr]0, [ImeProbe]::LoadKeyboardLayoutW('00000412', 0))
}
Step 'state' { [void][ImeProbe]::PostMessageW([ImeProbe]::Hwnd, 0x401, [IntPtr]0, [IntPtr]0) }
Step 'ko Hangul key (VK_HANGUL 0x15), r k' { [ImeProbe]::Keys(0x15, 0x52, 0x4B) | Out-Null }
Step 'state' { [void][ImeProbe]::PostMessageW([ImeProbe]::Hwnd, 0x401, [IntPtr]0, [IntPtr]0) }
Step 'ko r k s' { [ImeProbe]::Keys(0x52, 0x4B, 0x53) | Out-Null }
Step 'enter' { [ImeProbe]::Keys(0x0D) | Out-Null }

# The touch keyboard: TabTip's ITipInvocation.
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class TouchKbd {
  [ComImport, Guid("37c994e7-432b-4834-a2f7-dce1f13b834b"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface ITipInvocation { void Toggle(IntPtr hwnd); }
  [ComImport, Guid("4ce576fa-83dc-4F88-951c-9d0782b4e376")] class UIHostNoLaunch {}
  [DllImport("user32.dll")] static extern IntPtr GetDesktopWindow();
  public static string Toggle() {
    try { ((ITipInvocation) new UIHostNoLaunch()).Toggle(GetDesktopWindow()); return "toggled"; }
    catch (Exception e) { return e.GetType().Name + ": " + e.Message; }
  }
}
'@
Write-Host "touch keyboard toggle (TabTip not running): $([TouchKbd]::Toggle())"
Start-Process 'C:\Program Files\Common Files\microsoft shared\ink\TabTip.exe' -ErrorAction SilentlyContinue
Start-Sleep 3
Write-Host "processes: $((Get-Process TabTip, TextInputHost -ErrorAction SilentlyContinue | ForEach-Object { $_.Name + ' ' + $_.Id }) -join ', ')"
Write-Host "touch keyboard toggle: $([TouchKbd]::Toggle())"
Start-Sleep 3
Write-Host ([ImeProbe]::Windows())
exit 0
