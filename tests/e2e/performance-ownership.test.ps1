# Actual maintained RunWindow/CloseAndWait with explicit native-window boundaries.
# These are ownership/order contracts, not a claim of live Windows UI execution.
param([string] $Workload = (Join-Path $PSScriptRoot 'window-workload.ps1'))
$ErrorActionPreference = 'Stop'
$source = [IO.File]::ReadAllText((Resolve-Path $Workload))
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseInput($source, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw ($errors -join "`n") }
$analyzer = [regex]::Match($source, "(?s)Add-Type -TypeDefinition @'\r?\n(using System;.*?public static class GesherE2E .*?)\r?\n'@")
if (-not $analyzer.Success) { throw 'Missing maintained pixel analyzer' }
Add-Type -TypeDefinition $analyzer.Groups[1].Value
foreach ($name in @('Analyze', 'PixelDiff', 'AssertMeasuredWindow', 'RegisterMeasuredParticipants',
                    'CaptureSettled', 'AppPid', 'KillTree', 'CloseAndWait', 'RunWindow')) {
  $functions = @($ast.FindAll({ param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] }, $true) |
    Where-Object Name -eq $name)
  if ($functions.Count -ne 1) { throw "Missing maintained $name" }
  Invoke-Expression $functions[0].Extent.Text
}
Add-Type -TypeDefinition @'
using System;
public static class GesherWin32 {
  public static int Owner = System.Diagnostics.Process.GetCurrentProcess().Id;
  public static int Clicks, Keys, Captures, OwnedCloses, ForeignCloses;
  public static bool Foreign, RecycleBeforeInput;
  public static bool IsWindow(IntPtr hwnd) { return hwnd != IntPtr.Zero; }
  public static uint ProcessId(IntPtr hwnd) { return (uint)(Foreign || hwnd.ToInt64() == 2 ? Owner + 1000000 : Owner); }
  public static string Title(IntPtr hwnd) { if (RecycleBeforeInput) Foreign = true; return "Fixture"; }
  public static int[] Capture(IntPtr hwnd) {
    Captures++;
    var px = new int[402]; px[0] = 20; px[1] = 20;
    for (int i = 2; i < px.Length; i++) px[i] = i < 202 ? 0xffffff : 0;
    if (Clicks > 0) for (int i = 2; i < 102; i++) px[i] = 0x00ff00;
    return px;
  }
  public static void Click(IntPtr hwnd, int x, int y) { Clicks++; }
  public static void TypeKeys(IntPtr hwnd, string keys) { Keys++; }
  public static bool PostMessageW(IntPtr hwnd, uint message, IntPtr w, IntPtr l) {
    if (message == 0x10) { if (ProcessId(hwnd) == Owner) OwnedCloses++; else ForeignCloses++; }
    return true;
  }
}
'@
$script:checks = 0; $script:failures = 0
function Check([bool] $Condition, [string] $Name) {
  $script:checks++
  if (-not $Condition) { $script:failures++; Write-Host "OWNERSHIP_FAIL $Name" }
}
function ExpectFailure([scriptblock] $Action, [string] $Name) {
  $failed = $false
  try { & $Action | Out-Null } catch { $failed = $true; Write-Host "OWNERSHIP_REJECT $Name : $($_.Exception.Message)" }
  Check $failed $Name
}
function ResetBoundary {
  [GesherWin32]::Clicks = 0; [GesherWin32]::Keys = 0; [GesherWin32]::Captures = 0
  [GesherWin32]::OwnedCloses = 0; [GesherWin32]::ForeignCloses = 0
  [GesherWin32]::Foreign = $false; [GesherWin32]::RecycleBeforeInput = $false
  $script:PerfJobOwnsCleanup = $true
  $script:PerfVerifiedWindowProcesses = @{}
  $script:PerfParticipantCapture = $null
  $script:PerfRegisterParticipants = { param($Participants) return $true }
  $script:extraWindow = $false
  $script:clock = 0
  $script:fixtureProcess = [pscustomobject]@{ HasExited = $false; ExitCode = 0 }
  $script:fixtureProcess | Add-Member -MemberType ScriptMethod -Name WaitForExit -Value {}
}
function Get-Date { return ([datetime]'2026-01-01T00:00:00Z').AddMilliseconds($script:clock) }
function Start-Sleep([int] $Seconds = 0, [int] $Milliseconds = 0) {
  $script:clock += $Seconds * 1000 + $Milliseconds
  if ([GesherWin32]::OwnedCloses) { $script:fixtureProcess.HasExited = $true }
}
function StartLogged { return $script:fixtureProcess }
function WaitFirstFrame { return [IntPtr]1 }
function FrontWindow { return [IntPtr]1 }
function SavePng { }
function PngBase64 { return '' }
function LogMark { return @(0, 0) }
function RenderReadiness { return @{ Ready = $true } }
function ReadText {
  $text = "app process for ownership.fixture: pid $PID`nhwnd 0x1`nthe process of ownership.fixture ended`n"
  if ($script:extraWindow) { $text += "hwnd 0x2`n" }
  return $text
}
function Get-Process { return $null } # App termination boundary; never touches a real process.
$OutDir = [IO.Path]::GetTempPath()
$scenario = [pscustomobject]@{ package = 'ownership.fixture'; actions = @(@{ click = @(5, 5) }) }

ResetBoundary
$result = RunWindow $scenario 'owned-window-fixture' 'launch' -Measure
Check ($result.Problems.Count -eq 0 -and $result.Performance.InputVerified -and $result.Performance.LifecycleVerified) 'acknowledged identity permits the maintained workload'
Check ([GesherWin32]::Clicks -eq 1 -and [GesherWin32]::OwnedCloses -gt 0 -and [GesherWin32]::ForeignCloses -eq 0) 'owned input and close execute'

ResetBoundary
$script:PerfRegisterParticipants = { param($Participants) return $false }
ExpectFailure { RunWindow $scenario 'owned-window-fixture' 'launch' -Measure } 'missing registration acknowledgment fails before interaction'
Check ([GesherWin32]::Captures -eq 0 -and [GesherWin32]::Clicks -eq 0 -and [GesherWin32]::OwnedCloses -eq 0) 'unacknowledged window receives no interaction'

ResetBoundary
[GesherWin32]::RecycleBeforeInput = $true
ExpectFailure { RunWindow $scenario 'owned-window-fixture' 'launch' -Measure } 'actual RunWindow rejects an HWND recycled before input'
Check ([GesherWin32]::Clicks -eq 0 -and [GesherWin32]::Keys -eq 0 -and [GesherWin32]::ForeignCloses -eq 0) 'foreign input/close is never sent'

ResetBoundary
[GesherWin32]::Foreign = $true
ExpectFailure { CloseAndWait ([IntPtr]1) $script:fixtureProcess 'fixture.log' 'ownership.fixture' } 'foreign initial HWND is rejected'
Check ([GesherWin32]::ForeignCloses -eq 0) 'foreign initial HWND gets no WM_CLOSE'

ResetBoundary
$script:PerfVerifiedWindowProcesses[[int]$PID] = [Diagnostics.Process]::GetCurrentProcess().StartTime.ToFileTimeUtc() - 1
ExpectFailure { CloseAndWait ([IntPtr]1) $script:fixtureProcess 'fixture.log' 'ownership.fixture' } 'same PID with a stale process generation is rejected'
Check ([GesherWin32]::OwnedCloses -eq 0) 'stale process generation gets no WM_CLOSE'

ResetBoundary
$script:PerfVerifiedWindowProcesses[[int]$PID] = [Diagnostics.Process]::GetCurrentProcess().StartTime.ToFileTimeUtc()
$script:extraWindow = $true
ExpectFailure { CloseAndWait ([IntPtr]1) $script:fixtureProcess 'fixture.log' 'ownership.fixture' } 'recycled HWND retained in the log is rejected'
Check ([GesherWin32]::OwnedCloses -gt 0 -and [GesherWin32]::ForeignCloses -eq 0) 'each logged HWND is authenticated separately'

Write-Host "OWNERSHIP_DONE checks=$script:checks failures=$script:failures"
exit $(if ($script:failures) { 1 } else { 0 })
