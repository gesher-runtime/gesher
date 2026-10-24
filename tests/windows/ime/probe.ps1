# Temporary CI probe (ime track): the runners' IMEs, keyboard layouts and
# SendInput, outside Gesher.
$ErrorActionPreference = 'Continue'
Write-Host "OS: $((Get-CimInstance Win32_OperatingSystem).Caption) $([Environment]::OSVersion.Version)"
Write-Host "session: $([Diagnostics.Process]::GetCurrentProcess().SessionId), user $env:USERNAME, interactive $([Environment]::UserInteractive)"
Get-WinUserLanguageList | ForEach-Object { Write-Host "language $($_.LanguageTag): $($_.InputMethodTips -join ', ')" }
Get-ChildItem C:\Windows\System32\IME -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "IME dir: $($_.Name)" }
foreach ($p in 'C:\Program Files\Common Files\microsoft shared\ink\TabTip.exe', 'C:\Windows\SystemApps\MicrosoftWindows.Client.CBS_cw5n1h2txyewy\TextInputHost.exe') {
  Write-Host "exists $(Test-Path $p): $p"
}
Get-ChildItem 'HKLM:\SOFTWARE\Microsoft\CTF\TIP' -ErrorAction SilentlyContinue | ForEach-Object {
  $d = (Get-ItemProperty "$($_.PSPath)" -ErrorAction SilentlyContinue).'(default)'
  Write-Host "TIP $($_.PSChildName) $d"
}
Get-Command Install-Language -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "Install-Language available" }

Add-Type -Path "$PSScriptRoot/probe.cs"
[ImeProbe]::Start()
Start-Sleep 1
Write-Host "foreground: $([ImeProbe]::Foreground([ImeProbe]::Hwnd))"
Write-Host "layouts: $([ImeProbe]::Layouts())"
function Step($name, [scriptblock] $s) { & $s; Start-Sleep -Milliseconds 1500; Write-Host "--- $name"; Write-Host ([ImeProbe]::Log()) }
Step 'SendInput unicode' { [ImeProbe]::Unicode("A`u{05D0}`u{D83D}`u{DE00}") | Out-Null }
Step 'Hebrew layout, SendInput keys A B C' {
  $hkl = [ImeProbe]::LoadKeyboardLayoutW('0000040D', 0)
  Write-Host ("hebrew hkl {0:x}" -f $hkl.ToInt64())
  [void][ImeProbe]::PostMessageW([ImeProbe]::Hwnd, 0x50, [IntPtr]0, $hkl)
  Start-Sleep -Milliseconds 500
  [ImeProbe]::Keys(0x41, 0x42, 0x43) | Out-Null
}
Step 'state' { [void][ImeProbe]::PostMessageW([ImeProbe]::Hwnd, 0x401, [IntPtr]0, [IntPtr]0) }

foreach ($lang in @(@('ja-JP', '00000411', 'ja', 0x9), @('ko-KR', '00000412', 'ko', 0x1), @('zh-CN', '00000804', 'zh', 0x1))) {
  try {
    $list = Get-WinUserLanguageList
    $list.Add($lang[0])
    Set-WinUserLanguageList $list -Force -WarningAction SilentlyContinue
    Get-WinUserLanguageList | Where-Object LanguageTag -eq $lang[0] | ForEach-Object { Write-Host "added $($_.LanguageTag): $($_.InputMethodTips -join ', ')" }
  } catch { Write-Host "adding $($lang[0]): $_" }
  Write-Host "layouts: $([ImeProbe]::Layouts())"
  [ImeProbe]::Foreground([ImeProbe]::Hwnd) | Out-Null
  Step "$($lang[0]) layout request" {
    $hkl = [ImeProbe]::LoadKeyboardLayoutW($lang[1], 1)
    Write-Host ("hkl {0:x}" -f $hkl.ToInt64())
    [void][ImeProbe]::PostMessageW([ImeProbe]::Hwnd, 0x50, [IntPtr]0, $hkl)
  }
  [ImeProbe]::Tip = $lang[2]
  Step "$($lang[0]) TSF profile" { [void][ImeProbe]::PostMessageW([ImeProbe]::Hwnd, 0x404, [IntPtr]0, [IntPtr]0) }
  Step "$($lang[0]) open" { [void][ImeProbe]::PostMessageW([ImeProbe]::Hwnd, 0x402, [IntPtr]$lang[3], [IntPtr]0) }
  Step "$($lang[0]) state" { [void][ImeProbe]::PostMessageW([ImeProbe]::Hwnd, 0x401, [IntPtr]0, [IntPtr]0) }
  Step "$($lang[0]) keys n i h o n r k space enter" { [ImeProbe]::Keys(0x4E, 0x49, 0x48, 0x4F, 0x4E, 0x52, 0x4B, 0x20, 0x0D) | Out-Null }
  Step "$($lang[0]) SETSTR" { [void][ImeProbe]::PostMessageW([ImeProbe]::Hwnd, 0x403, [IntPtr]0, [IntPtr]0) }
}
exit 0
