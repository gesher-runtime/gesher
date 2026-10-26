# Text input like a native Windows app's (docs/input.md "Text input", ADR
# 0039). Dot-sourced by run-tests.ps1 after 34-windows.ps1.
#
# imeapp.apk (tests/apps/imeapp) runs with a window shown on the desktop,
# brought to the foreground, and is typed into with real keyboard input
# (SendInput): Unicode text (VK_PACKET, as the emoji panel and dictation type),
# editing keys and the clipboard shortcuts, keys under a Hebrew keyboard
# layout (the window's thread switched with WM_INPUTLANGCHANGEREQUEST), Enter
# in editors whose IME action is done, search and next, Microsoft IME for
# Japanese composing and converting (its candidate window placed at the
# caret), a password field. The app reports its editors' text, composing
# region and cursor and what InputMethodManager answers; the window thread's
# caret is read with GetGUIThreadInfo. Twice: a single process, and the Gesher
# device (the app process the system server starts).
. "$PSScriptRoot/../graphics/WindowChecks.ps1"
if (-not ('ImeInput' -as [type])) { Add-Type -Path "$PSScriptRoot/../ime/ImeInput.cs" }

$bcpIme = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$dvmIme = @("-Xbootclasspath:$bcpIme", "-Ximage:$(NoDrive "$root/system")/framework/boot.art", '-Xint', '-Xusejit:false')

# One check of a run: PASS or FAIL (counted) with what was seen.
function ImeCheck([string] $Mode, [string] $Name, [bool] $Ok, [string] $Seen) {
  if ($Ok) {
    Write-Host "PASS  ime: $Name ($Mode)$(if ($Seen) { ": $Seen" })"
  } else {
    Write-Host "FAIL  ime: $Name ($Mode)$(if ($Seen) { ": $Seen" })"
    $script:failed++
    $script:imeFailed = $true
  }
}

# The last text imeapp reported for editor $Editor ('' when none).
function ImeText([scriptblock] $ReadLog, [string] $Editor) {
  $m = [regex]::Matches("$(& $ReadLog)", "ImeActivity text ${Editor}: ([^\r\n]*)")
  if ($m.Count -eq 0) { return '' }
  return $m[$m.Count - 1].Groups[1].Value
}

# Waits until editor $Editor's last reported text is $Want (escaped as the app
# logs it); returns what it last was.
function WaitText([scriptblock] $ReadLog, [string] $Editor, [string] $Want, [int] $Seconds = 30) {
  $deadline = (Get-Date).AddSeconds($Seconds)
  do {
    $t = ImeText $ReadLog $Editor
    if ($t -ceq $Want) { return $t }
    Start-Sleep -Milliseconds 200
  } while ((Get-Date) -lt $deadline)
  return $t
}

# The number of times $Pattern (a regex) is in the log.
function LogCount([scriptblock] $ReadLog, [string] $Pattern) {
  return [regex]::Matches("$(& $ReadLog)", $Pattern).Count
}

function WaitCount([scriptblock] $ReadLog, [string] $Pattern, [int] $Count, [int] $Seconds = 30) {
  $deadline = (Get-Date).AddSeconds($Seconds)
  do {
    if ((LogCount $ReadLog $Pattern) -ge $Count) { return $true }
    Start-Sleep -Milliseconds 200
  } while ((Get-Date) -lt $deadline)
  return $false
}

# Switches the keyboard layout of $hwnd's thread (KLID $Klid, language
# $Lang), asking again until it has it (Windows may take a while, or answer
# an earlier request late).
function SwitchTo([IntPtr] $hwnd, [string] $Klid, [int] $Lang) {
  $deadline = (Get-Date).AddSeconds(30)
  do {
    [void][ImeInput]::SwitchLayout($hwnd, $Klid)
    $until = (Get-Date).AddSeconds(2)
    while ((([ImeInput]::ThreadLayout($hwnd) -band 0xffff) -ne $Lang) -and (Get-Date) -lt $until) {
      Start-Sleep -Milliseconds 200
    }
  } while ((([ImeInput]::ThreadLayout($hwnd) -band 0xffff) -ne $Lang) -and (Get-Date) -lt $deadline)
  Start-Sleep -Milliseconds 500
}

# The checks of one run, on imeapp's window $hwnd (process $ProcessId).
function ImeChecks([string] $Mode, [IntPtr] $hwnd, [uint32] $ProcessId, [scriptblock] $ReadLog) {
  $fg = [ImeInput]::Foreground($hwnd)
  ImeCheck $Mode 'imeapp.apk''s window is the foreground window (SendInput reaches it)' $fg ''
  if (-not $fg) { return }

  # InputMethodManager in the app, once the window has the focus.
  $ok = WaitForLog $ReadLog 'ImeActivity imm:' 60
  $imm = [regex]::Match("$(& $ReadLog)", 'ImeActivity imm:[^\r\n]*').Value
  $ok = $ok -and $imm.Contains('active true, accepting true') -and
        $imm.Contains('methods [com.gesher.app/.input.WindowsInputMethod ''Windows text input''') -and
        $imm.Contains('current com.gesher.app/.input.WindowsInputMethod') -and
        $imm.Contains('showSoftInput true') -and
        $imm.Contains('restartInput: active true') -and
        "$(& $ReadLog)".Contains('gesher-ime: input started in window "Gesher IME test"')
  ImeCheck $Mode 'InputMethodManager: the focused EditText is active and accepting text, the input method is Windows text input with the keyboard layouts as subtypes, showSoftInput/hideSoftInputFromWindow/restartInput' $ok $imm
  $soft = [regex]::Matches("$(& $ReadLog)", 'gesher-ime: soft input [^\r\n]*') | ForEach-Object { $_.Value } | Select-Object -First 2
  Write-Host "INFO  ime: $($soft -join ' / ')"

  # Unicode input (VK_PACKET): Hebrew and an emoji (a surrogate pair).
  [void][ImeInput]::Unicode("$([char]0x05E9)$([char]0x05DC)$([char]0x05D5)$([char]0x05DD) $([char]0xD83D)$([char]0xDE00)")
  $t0 = '\u05e9\u05dc\u05d5\u05dd \ud83d\ude00'
  $t = WaitText $ReadLog 'edit' $t0
  ImeCheck $Mode 'SendInput Unicode text (VK_PACKET, as the emoji panel and dictation type) is committed: Hebrew and an emoji' ($t -ceq $t0) $t

  # Editing keys and the clipboard: Ctrl+A, Ctrl+C, End, Ctrl+V, Ctrl+Z, Ctrl+X, Backspace, Delete.
  $ctrl = [uint16[]]@(0x11)
  [void][ImeInput]::Chord($ctrl, 0x41); [void][ImeInput]::Chord($ctrl, 0x43); [void][ImeInput]::Keys(0x23)
  [void][ImeInput]::Chord($ctrl, 0x56)
  $t = WaitText $ReadLog 'edit' "$t0$t0"
  $steps = @("Ctrl+A Ctrl+C End Ctrl+V: $t")
  $ok = $t -ceq "$t0$t0"
  [void][ImeInput]::Chord($ctrl, 0x5A)
  $t = WaitText $ReadLog 'edit' $t0
  $steps += "Ctrl+Z: $t"; $ok = $ok -and $t -ceq $t0
  [void][ImeInput]::Chord($ctrl, 0x41); [void][ImeInput]::Chord($ctrl, 0x58)
  $t = WaitText $ReadLog 'edit' ''
  $steps += "Ctrl+A Ctrl+X: '$t'"; $ok = $ok -and $t -ceq ''
  [void][ImeInput]::Chord($ctrl, 0x56)
  $t = WaitText $ReadLog 'edit' $t0
  $steps += "Ctrl+V: $t"; $ok = $ok -and $t -ceq $t0
  [void][ImeInput]::Keys(0x08)
  $t = WaitText $ReadLog 'edit' '\u05e9\u05dc\u05d5\u05dd '
  $steps += "Backspace: $t"; $ok = $ok -and $t -ceq '\u05e9\u05dc\u05d5\u05dd '
  [void][ImeInput]::Chord($ctrl, 0x41); [void][ImeInput]::Keys(0x2E)
  $t = WaitText $ReadLog 'edit' ''
  $steps += "Ctrl+A Delete: '$t'"; $ok = $ok -and $t -ceq ''
  ImeCheck $Mode 'editing keys and clipboard shortcuts in the EditText (Ctrl+A/C/V/Z/X, End, Backspace, Delete)' $ok ($steps -join '; ')

  # A Hebrew keyboard layout: the window's thread switched, keys A B C type Hebrew.
  SwitchTo $hwnd '0000040D' 0x040d
  $layout = '{0:x}' -f [ImeInput]::ThreadLayout($hwnd)
  [void][ImeInput]::Keys(0x41, 0x42, 0x43)
  $t = WaitText $ReadLog 'edit' '\u05e9\u05e0\u05d1'
  $ok = $t -ceq '\u05e9\u05e0\u05d1'
  [void][ImeInput]::Keys(0x74)  # F5: the app reports InputMethodManager again
  $ok2 = WaitCount $ReadLog 'ImeActivity imm:[^\r\n]*subtype he' 1 20
  ImeCheck $Mode 'a Hebrew keyboard layout (LoadKeyboardLayout 0000040D, the window''s thread switched): keys A, B, C type Hebrew letters, not Latin ones; the current subtype is he' ($ok -and $ok2) "layout $layout, text $t, subtype he: $ok2"
  SwitchTo $hwnd '00000409' 0x0409
  [void][ImeInput]::Chord($ctrl, 0x41); [void][ImeInput]::Keys(0x2E)
  [void](WaitText $ReadLog 'edit' '')

  # The caret: the system caret of the window's thread is the editor's cursor.
  [void][ImeInput]::Keys(0x41, 0x42, 0x43)
  $t = WaitText $ReadLog 'edit' 'abc'
  Start-Sleep -Seconds 1
  $m = [regex]::Matches("$(& $ReadLog)", 'ImeActivity cursor edit: (-?\d+),(-?\d+)-(-?\d+)')
  $caret = [ImeInput]::Caret($hwnd)
  $ok = $t -ceq 'abc' -and $m.Count -gt 0 -and $null -ne $caret
  $seen = "text $t"
  if ($ok) {
    $last = $m[$m.Count - 1]
    $cx = [int]$last.Groups[1].Value; $ctop = [int]$last.Groups[2].Value
    $seen = "the editor's cursor at $cx,$ctop; the thread's caret at $($caret[0]),$($caret[1]) (screen $($caret[4]),$($caret[5]))"
    $ok = [Math]::Abs($caret[0] - $cx) -le 3 -and [Math]::Abs($caret[1] - $ctop) -le 3
  }
  ImeCheck $Mode 'the window thread''s caret (GetGUIThreadInfo) is the editor''s cursor (CursorAnchorInfo)' $ok $seen

  # Enter performs the editor's IME action (performEditorAction).
  [void][ImeInput]::Keys(0x0D)
  $ok = (WaitForLog $ReadLog 'ImeActivity action edit: 6' 20) -and "$(& $ReadLog)".Contains('gesher-ime: editor action 6 (Enter)')
  $seen = @("done: $ok")
  [GesherWin32]::Click($hwnd, 200, 90)
  $okSearch = WaitForLog $ReadLog 'ImeActivity focus search' 20
  [void][ImeInput]::Keys(0x58, 0x0D)
  $okSearch = $okSearch -and (WaitForLog $ReadLog 'ImeActivity action search: 3' 20)
  $seen += "search: $okSearch"
  [GesherWin32]::Click($hwnd, 200, 150)
  $okNext = WaitForLog $ReadLog 'ImeActivity focus next' 20
  [void][ImeInput]::Keys(0x0D)
  $okNext = $okNext -and (WaitForLog $ReadLog 'ImeActivity action next: 5' 20) -and (WaitForLog $ReadLog 'ImeActivity focus last' 20)
  $seen += "next (focus moves on): $okNext"
  ImeCheck $Mode 'Enter performs the editor''s IME action through the InputConnection: IME_ACTION_DONE, IME_ACTION_SEARCH, IME_ACTION_NEXT (focus moves to the next field)' ($ok -and $okSearch -and $okNext) ($seen -join ', ')

  # Microsoft IME for Japanese in the 'last' editor: compose, convert, commit.
  SwitchTo $hwnd '00000411' 0x0411
  $jaLayout = '{0:x}' -f [ImeInput]::ThreadLayout($hwnd)
  Start-Sleep -Seconds 2
  [void][ImeInput]::Keys(0x19)  # VK_KANJI: the IME on
  Start-Sleep -Seconds 2
  [void][ImeInput]::Keys(0x4E, 0x49, 0x48, 0x4F, 0x4E, 0x4E)
  $composed = WaitCount $ReadLog 'ImeActivity text last: [^\r\n]*composing [^\r\n]*\\u306b\\u307b\\u3093' 1 120
  $compose = [regex]::Matches("$(& $ReadLog)", 'ImeActivity text last: [^\r\n]*composing[^\r\n]*') | ForEach-Object { $_.Value } | Select-Object -Last 1
  [void][ImeInput]::Keys(0x20)  # convert
  $converted = WaitCount $ReadLog 'ImeActivity text last: [^\r\n]*composing [^\r\n]*''\\u[4-9][0-9a-f]{3}' 1 180
  [void][ImeInput]::Keys(0x20)  # the candidate list
  # Windows Server 2022's Microsoft IME may take a minute to show it.
  $deadline = (Get-Date).AddSeconds(150)
  do {
    Start-Sleep -Seconds 2
    $windows = [ImeInput]::ProcessWindows($ProcessId)
  } while (-not ($windows | Where-Object { $_ -match 'cand' }) -and (Get-Date) -lt $deadline)
  Start-Sleep -Seconds 1
  $windows = [ImeInput]::ProcessWindows($ProcessId)
  $caret = [ImeInput]::Caret($hwnd)
  # Android floating IMEs do not create edge insets. Observe the real Microsoft
  # candidate while it is still visible, without moving focus or committing.
  $candidateBeforeCommit = $windows | Where-Object { $_ -match 'cand' } | Select-Object -First 1
  $candidateFloating = $false
  if ($candidateBeforeCommit -and $null -ne $caret) {
    $candidateBounds = ($candidateBeforeCommit -split '@')[1] -split ','
    $client = [GesherWin32]::ClientSize($hwnd)
    $clientBottom = $caret[5] - $caret[1] + $client[1]
    $candidateFloating = [int]$candidateBounds[1] + [int]$candidateBounds[3] -lt $clientBottom
    $insetPattern = "ImeActivity composing IME insets 0,0,0,0 decor $($client[0])x$($client[1])"
    $beforeInsets = LogCount $ReadLog $insetPattern
    $noneWhileVisible = WaitCount $ReadLog $insetPattern ($beforeInsets + 1) 5
    ImeCheck $Mode 'a real floating Microsoft candidate window keeps Android IME edge insets empty and content unresized' ($candidateFloating -and $noneWhileVisible) "candidate $candidateBeforeCommit, native client $($client -join 'x'), new composing inset snapshot $noneWhileVisible"
  } else {
    ImeCheck $Mode 'a real floating Microsoft candidate window keeps Android IME edge insets empty and content unresized' $false 'candidate/caret unavailable'
  }
  $commitMark = "$(& $ReadLog)".Length
  [void][ImeInput]::Keys(0x0D)  # commit
  $t = ''
  $nativeCommit = ''
  $committed = $false
  $deadline = (Get-Date).AddSeconds(150)
  do {
    $t = ImeText $ReadLog 'last'
    $all = "$(& $ReadLog)"
    $afterCommit = if ($all.Length -gt $commitMark) { $all.Substring($commitMark) } else { '' }
    $results = [regex]::Matches($afterCommit, 'gesher-ime: commit "([^"\r\n]+)"')
    if ($results.Count -gt 0) { $nativeCommit = $results[$results.Count - 1].Groups[1].Value }
    # Candidate ordering depends on the runner's Microsoft IME dictionary:
    # both two- and three-kanji selections are valid. Require fidelity to the
    # actual IMM result, all kanji, and no remaining composing region instead
    # of assuming a particular dictionary word length.
    $committed = $t -match '^(\\u[4-9][0-9a-f]{3})+$' -and $t -ceq $nativeCommit
    if ($committed) { break }
    Start-Sleep -Milliseconds 200
  } while ((Get-Date) -lt $deadline)
  ImeCheck $Mode 'IME composition (Microsoft IME, Japanese, typed with SendInput): setComposingText while composing (nihon, as hiragana), converted to kanji, Enter preserves the exact IMM result with no composing region left' ($composed -and $converted -and $committed) ("TSF 0x{0:x8}, layout $jaLayout; composing: $composed ($compose); converted: $converted; native result: $nativeCommit; committed: $t" -f $script:imeTsf)
  $cand = $windows | Where-Object { $_ -match 'cand' } | Select-Object -First 1
  $inline = -not ($windows | Where-Object { $_ -match 'Composition' })
  if ($cand -and $null -ne $caret) {
    $r = ($cand -split '@')[1] -split ','
    $near = [Math]::Abs([int]$r[0] - $caret[4]) -le 120 -and [int]$r[1] -ge $caret[5] -and [int]$r[1] -le ($caret[5] + 120)
    ImeCheck $Mode 'the IME''s candidate window is placed at the caret (ImmSetCandidateWindow, CFS_EXCLUDE), the composition inline (no composition window)' ($near -and $inline) "candidate window $cand, caret on screen $($caret[4]),$($caret[5]); windows: $($windows -join ' ')"
  } else {
    ImeCheck $Mode 'the IME''s candidate window is placed at the caret (ImmSetCandidateWindow, CFS_EXCLUDE), the composition inline (no composition window)' $false "no candidate window ($($windows -join ' ')), caret $($caret -join ',')"
  }
  EndImeBroker 'after the composition'
  [void][ImeInput]::Keys(0x19)  # the IME off
  SwitchTo $hwnd '00000409' 0x0409
  Start-Sleep -Seconds 1

  # A password field takes keys, and no compositions.
  [GesherWin32]::Click($hwnd, 200, 270)
  $ok = WaitForLog $ReadLog 'ImeActivity focus password' 20
  [void][ImeInput]::Keys(0x50, 0x57)
  $t = WaitText $ReadLog 'password' 'pw' 120
  $ok = $ok -and $t -ceq 'pw' -and ("$(& $ReadLog)" -match 'gesher-ime: input started in window "Gesher IME test": inputType 0x81[^\r\n]*compositions false')
  ImeCheck $Mode 'a password field: typed into, no IME compositions' $ok "text $t"
}

# Runs imeapp.apk with dalvikvm $Arguments until its window's first frame,
# runs ImeChecks on it, closes it (WM_CLOSE) and expects $Expect in the output.
function ImeAppRun([string] $Mode, [string[]] $Arguments, [string[]] $Expect, [int] $Seconds = 400) {
  $script:imeFailed = $false
  EndImeBroker 'before the run'
  if (-not (Test-Path "$Dir/imeapp.apk")) {
    ImeCheck $Mode 'imeapp.apk runs' $false 'imeapp.apk is missing'
    return
  }
  $log = Join-Path $env:RUNNER_TEMP "ime-app-$(Get-Random).log"
  $quoted = (@($dvmIme) + $Arguments | ForEach-Object { if ($_ -match '[\s"]') { '"' + $_ + '"' } else { $_ } }) -join ' '
  $psi = [Diagnostics.ProcessStartInfo]::new('cmd.exe', "/d /s /c `"`"$Dir/dalvikvm.exe`" $quoted > `"$log`" 2>&1`"")
  $psi.Environment['GESHER_TRACE_FAULTS'] = '1'
  $psi.UseShellExecute = $false
  $p = [Diagnostics.Process]::Start($psi)
  $readLog = { Get-Content -Raw $log -ErrorAction SilentlyContinue }.GetNewClosure()
  $hwnd = [IntPtr]::Zero
  $deadline = (Get-Date).AddSeconds($Seconds)
  while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
    $m = [regex]::Match("$(& $readLog)", 'first frame drawn \((\d+) presented\), \w+, hwnd 0x([0-9a-f]+)')
    if ($m.Success) { $hwnd = [IntPtr][Convert]::ToInt64($m.Groups[2].Value, 16); break }
    Start-Sleep -Milliseconds 250
  }
  if ($hwnd -eq [IntPtr]::Zero) {
    ImeCheck $Mode 'imeapp.apk''s window is drawn' $false 'no first frame'
  } else {
    Start-Sleep -Seconds 2
    ImeChecks $Mode $hwnd ([GesherWin32]::ProcessId($hwnd)) $readLog
    [void][GesherWin32]::PostMessageW($hwnd, 0x0010 <# WM_CLOSE #>, [IntPtr]::Zero, [IntPtr]::Zero)
  }
  $problems = @()
  if (-not $p.WaitForExit(120000)) { $p.Kill($true); $p.WaitForExit(); $problems += 'did not exit after WM_CLOSE' }
  elseif ($p.ExitCode -ne 0) { $problems += ('exit 0x{0:X8}' -f $p.ExitCode) }
  $text = "$(& $readLog)"
  foreach ($want in $Expect) {
    if (-not $text.Contains($want)) { $problems += "missing '$want'" }
  }
  ImeCheck $Mode 'imeapp.apk runs, its window closes with WM_CLOSE' (-not $problems) ($problems -join '; ')
  # The window thread's long waits (thread_watch.cc, ime.cc's SlowWatch).
  ($text -split "`n" | Select-String -Pattern 'gesher-ime: (the thread of window| +#| +thread \d+:| +waits| +process \d+| +\(|.* took \d+ ms)') | Select-Object -First 1500 | Out-Host
  if ($script:imeFailed) {
    ($text -split "`n" | Select-String -Pattern '(gesher-ime|gesher-app: window|ImeActivity|Exception|FATAL| at )') | Select-Object -Last 150 | Out-Host
  } else {
    ($text -split "`n" | Select-String -Pattern 'gesher-ime: (input started|compose|commit|editor action|caret|soft input)') | Select-Object -First 40 | Out-Host
  }
}

# Microsoft IME for Japanese enabled for the user, the layouts loaded, before
# the runs (enabling a profile changes the session's input languages).
# Windows Server 2022 (build 20348): Microsoft IME's out-of-process side,
# ImeBroker.exe, hangs. Its prediction engine (imjppred.dll) waits in
# Windows.CloudStore, which on those runners never answers; the IME's worker
# thread in the app waits for its COM call into ImeBroker.exe, and the
# window's thread for that worker whenever the IME is switched away from
# (WM_INPUTLANGCHANGEREQUEST): 180 s each time, the IME's own timeout, as in
# any app's window (thread_watch.cc's stacks and wait chains, docs/input.md).
# History-based predictive input off and the previous IME version (EAIME.admx
# user policies) changed nothing. There the test ends ImeBroker.exe once a
# composition is done (the worker's call then fails at once) and before each
# run (COM starts it again when the IME needs it).
$script:imeBrokerHangs = [Environment]::OSVersion.Version.Build -lt 26100
function EndImeBroker([string] $When) {
  if (-not $script:imeBrokerHangs) { return }
  foreach ($b in @(Get-Process -Name ImeBroker -ErrorAction SilentlyContinue)) {
    try { $b.Kill(); [void]$b.WaitForExit(10000); Write-Host "INFO  ime: ImeBroker.exe (pid $($b.Id)) ended $When (Server 2022)" }
    catch { Write-Host "INFO  ime: ImeBroker.exe (pid $($b.Id)) could not be ended: $($_.Exception.Message)" }
  }
}
$script:imeTsf = [ImeInput]::EnableJapaneseIme()
# What runs of the text input services (the IME's out-of-process helpers).
$imeProcs = Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -match 'ctfmon|TextInput|TabTip|InputApp|IME|imjp|Broker|ShellExperience|explorer|sihost|svchost' } |
  Group-Object ProcessName | ForEach-Object { "$($_.Name) x$($_.Count)" }
$imeSvcs = Get-Service -Name TabletInputService, TextInputManagementService, CDPUserSvc*, cbdhsvc* -ErrorAction SilentlyContinue | ForEach-Object { "$($_.Name)=$($_.Status)" }
Write-Host "INFO  ime: processes: $($imeProcs -join ', '); services: $($imeSvcs -join ', ')"
foreach ($klid in '0000040D', '00000411', '00000409') { [void][ImeInput]::SwitchLayout([IntPtr]::Zero, $klid) }
Start-Sleep -Seconds 3

# 1. A single process: gesher-app --window (shown: SendInput and the IME's
#    windows need the window on the desktop).
ImeAppRun 'single process' @('com.gesher.app.Main', '--window', '--display', '400x600@160', (NoDrive "$Dir/imeapp.apk")) `
  @('ImeActivity onResume', 'window "Gesher IME test" added', 'input attached', 'closed by the user',
    'ImeActivity onDestroy', 'gesher-app: done')

# 2. The Gesher device: the app process the system server starts has the window
#    and its input method (a device of its own, as 34-windows.ps1's).
$savedData = $env:ANDROID_DATA
$savedLogs = $env:GESHER_LOGS
$env:ANDROID_DATA = NoDrive "$root/data-ime"
$env:GESHER_LOGS = NoDrive "$root/logs-ime"
$env:GESHER_DISPLAY = '400x600@160'
New-Item -ItemType Directory -Force "$root/data-ime" | Out-Null
$env:GESHER_SHORTCUTS = '0'
try {
  ImeAppRun 'the Gesher device' @('com.gesher.app.Launcher', 'launch', (NoDrive "$Dir/imeapp.apk"), '--window') `
    @('gesher-app: app process for com.gesher.imeapp: pid ', 'gesher-app: windows: Win32 windows of this process (window)',
      'ImeActivity onResume', 'window "Gesher IME test" added', 'input attached', 'ImeActivity onDestroy',
      'gesher-app: done')
} finally {
  $shutdownLog = Join-Path $env:RUNNER_TEMP 'ime-device-shutdown.log'
  $sd = Start-Process "$Dir/dalvikvm.exe" -ArgumentList (@($dvmIme) + 'com.gesher.app.Launcher', 'shutdown') `
    -NoNewWindow -PassThru -RedirectStandardOutput $shutdownLog -RedirectStandardError "$shutdownLog.err"
  if (-not $sd.WaitForExit(120000)) { $sd.Kill($true); Write-Host 'INFO  the device shutdown timed out' }
  if ($script:imeFailed) {
    foreach ($f in Get-ChildItem "$root/logs-ime" -Filter *.log -ErrorAction SilentlyContinue) {
      Write-Host "--- $($f.Name)"
      Get-Content $f.FullName -ErrorAction SilentlyContinue | Select-Object -Last 60 | Out-Host
    }
  }
  $env:ANDROID_DATA = $savedData
  if ($savedLogs) { $env:GESHER_LOGS = $savedLogs } else { Remove-Item Env:GESHER_LOGS -ErrorAction SilentlyContinue }
  Remove-Item Env:GESHER_DISPLAY, Env:GESHER_SHORTCUTS -ErrorAction SilentlyContinue
}
