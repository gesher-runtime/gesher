# Execute the maintained capture/input/close control flow with a deterministic
# clock and an explicit external-window boundary. This is a fixture-ordering
# test, not a WebView/Win32 implementation or a claim about rendering on Windows.
# Runs on PowerShell 7 and Windows PowerShell 5.1.
param(
  [string] $Runner = '',
  [string] $Manifest = '',
  [string] $CapturedFrames = ''
)
$ErrorActionPreference = 'Stop'
$here = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
if (-not $Runner) { $Runner = Join-Path $here 'run-e2e.ps1' }
if (-not $Manifest) { $Manifest = Join-Path $here 'apps.json' }
$source = [IO.File]::ReadAllText((Resolve-Path $Runner))
$tokens = $null; $parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseInput($source, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw ($parseErrors -join "`n") }
$analyzer = [regex]::Match($source, "(?s)Add-Type -TypeDefinition @'\r?\n(using System;.*?public static class GesherE2E .*?)\r?\n'@")
if (-not $analyzer.Success) { throw 'The maintained pixel analyzer was not found' }
Add-Type -TypeDefinition $analyzer.Groups[1].Value
foreach ($name in @('Analyze', 'PixelDiff', 'AppLogPath', 'LogMark', 'LogsSince', 'RenderReadiness', 'CaptureSettled', 'RunWindow')) {
  $function = @($ast.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] }, $true) |
    Where-Object Name -eq $name)
  # An old-source negative control lacks the newer helpers; its actual old
  # capture/close functions still run and must fail the same checks.
  if ($function.Count -eq 0 -and $name -in @('RenderReadiness', 'AppLogPath', 'LogMark', 'LogsSince')) { continue }
  if ($function.Count -ne 1) { throw "Expected one maintained $name function" }
  Invoke-Expression $function[0].Extent.Text
}
Add-Type -TypeDefinition @'
using System;
using System.IO;
public static class GesherWin32 {
  public static long Millis, PageAt, RenderAt;
  public static int Stage;
  public static bool IgnoreAccept;
  public static int[] Initial, EmptyEditor, Edited, EditedButton, Blank, Rendered;
  public static int[] Load(string path) {
    byte[] bytes = File.ReadAllBytes(path);
    var px = new int[bytes.Length / 4]; Buffer.BlockCopy(bytes, 0, px, 0, bytes.Length); return px;
  }
  // 400x180: the toolbar, the editor's typed line or the preview's heading in the
  // content rectangle, the floating button right of it.
  static int[] Frame(bool editor, bool heading, bool editing, bool white) {
    const int w = 400, h = 180;
    var px = new int[w * h + 2]; px[0] = w; px[1] = h;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++)
      px[2 + y*w + x] = y < 56 ? 0x202020 : white ? 0xffffff : 0xfafafa;
    // Plenty of toolbar text makes a whole-window predicate pass even while the
    // separate content rectangle remains blank.
    for (int y = 20; y < 30; y++) for (int x = 50; x < 100; x++) px[2+y*w+x] = 0xffffff;
    for (int y = 140; y < 160; y++) for (int x = 330; x < 350; x++)
      px[2+y*w+x] = editing ? 0x101010 : 0x00ddff;
    if (editor) for (int y = 74; y < 79; y++) for (int x = 12; x < 47; x++) px[2+y*w+x] = 0x202020;
    if (heading) for (int y = 84; y < 104; y++) for (int x = 12; x < 92; x++) px[2+y*w+x] = 0x202020;
    return px;
  }
  public static void Frames(string directory) {
    Initial = directory.Length == 0 ? Frame(false, false, false, false) : Load(Path.Combine(directory, "blank.bin"));
    EmptyEditor = directory.Length == 0 ? Frame(false, false, true, false) : Load(Path.Combine(directory, "initial.bin"));
    Edited = directory.Length == 0 ? Frame(true, false, true, false) : Load(Path.Combine(directory, "edited.bin"));
    Blank = directory.Length == 0 ? Frame(false, false, false, true) : Load(Path.Combine(directory, "blank.bin"));
    Rendered = directory.Length == 0 ? Frame(false, true, false, false) : Load(Path.Combine(directory, "rendered.bin"));
    // The button changed, the editor (and its text) stayed: the window changes outside
    // the content rectangle only.
    EditedButton = directory.Length == 0 ? Frame(true, false, false, false) : Load(Path.Combine(directory, "edited-button.bin"));
  }
  public static int[] Capture(IntPtr hwnd) {
    if (Stage == 4) return EditedButton;
    if (Stage == 3) return Millis >= RenderAt ? Rendered : Blank;
    return Stage == 2 ? Edited : Stage == 1 ? EmptyEditor : Initial;
  }
  public static void Click(IntPtr hwnd, int x, int y) {
    if (x == 96 && y == 75) { Stage = 1; return; }
    if (Stage == 0) { Stage = 1; return; }
    // The accept button; one that does not show the preview leaves the editor.
    Stage = IgnoreAccept ? 4 : 3;
  }
  public static void TypeKeys(IntPtr hwnd, string keys) { Stage = 2; }
  public static string Title(IntPtr hwnd) { return "Untitled.md"; }
  public static bool IsWindow(IntPtr hwnd) { return true; }
}
'@
[GesherWin32]::Frames($CapturedFrames)
$script:baseTime = [datetime]'2026-01-01T00:00:00Z'
function Get-Date { return $script:baseTime.AddMilliseconds([GesherWin32]::Millis) }
function Start-Sleep([int] $Seconds = 0, [int] $Milliseconds = 0) {
  [GesherWin32]::Millis += 1000 * $Seconds + $Milliseconds
}
function StartLogged { return @{ HasExited = $false } }
function WaitFirstFrame { return [IntPtr]1 }
function FrontWindow { return [IntPtr]1 }
function DismissStartupDialog { return $true }
function SavePng { }
# The logs grow: the startup page load at once, the action's own one at PageAt.
function ReadText([string] $file) {
  $text = "gesher-webview: first page loaded: file:///actual/`ngesher-webview: page 1 finished (file, ok)`n"
  if ([GesherWin32]::Millis -ge [GesherWin32]::PageAt) { $text += "gesher-webview: page 2 finished (file, ok)`n" }
  return $text
}
function CloseAndWait {
  $script:closeAt = [GesherWin32]::Millis
  $script:closes++
  return @()
}
function KillTree { $script:kills++ }
$OutDir = [IO.Path]::GetTempPath()
$script:checks = 0; $script:failures = 0
function Check([bool] $condition, [string] $name) {
  $script:checks++
  if (-not $condition) { $script:failures++; Write-Host "READINESS_FAIL $name" }
}
$notes = (Get-Content -Raw $Manifest | ConvertFrom-Json).apps | Where-Object package -eq 'org.billthefarmer.notes'
if ($null -eq $notes) { throw 'Pinned Notes scenario not found' }
function Case([string] $name, [long] $pageAt, [long] $renderAt, [bool] $pass, [switch] $Normal, [switch] $NoInput,
              [switch] $IgnoreAccept) {
  $a = $notes | ConvertTo-Json -Depth 20 | ConvertFrom-Json
  if ($Normal) { $a.startupDialog = $null }
  [GesherWin32]::Millis = 0; [GesherWin32]::Stage = 0
  [GesherWin32]::PageAt = $pageAt; [GesherWin32]::RenderAt = $renderAt
  [GesherWin32]::IgnoreAccept = $IgnoreAccept.IsPresent
  $script:closeAt = -1; $script:closes = 0; $script:kills = 0
  $result = RunWindow $a 'owned-test-process' 'launch' -NoInput:$NoInput
  $count = if ($Normal) { $a.actions.Count } else { $a.startupDialog.actions.Count }
  # Three-second startup + one initial stable capture; every prior action has
  # its original two-second delay + one stable capture. Only the final action
  # may consume its ORIGINAL 20-second capture budget, including all predicates.
  $captureStart = 4 + 3 * ($count - 1) + 2
  $deadline = if ($NoInput) { 4000 } else { 1000 * ($captureStart + 20) }
  $ready = $pageAt -le $script:closeAt -and $renderAt -lt $script:closeAt
  Check ($script:closes -eq 1 -and $script:kills -eq 0) "$name uses normal close exactly once"
  Check ($script:closeAt -le $deadline) "$name never extends or resets the original capture deadline"
  if ($NoInput) {
    Check ($script:closeAt -eq 4000 -and $result.Problems.Count -eq 0) "$name remains unchanged"
  } elseif ($pass) {
    Check ($result.Problems.Count -eq 0) "$name accepts completed rendered content"
    Check $ready "$name closes only after completion and rendered-content stability"
  } else {
    Check (@($result.Problems -match 'rendered content did not become ready').Count -gt 0) "$name fails missing readiness or content"
    Check ($script:closeAt -eq $deadline) "$name spends at most the one existing bounded wait"
  }
  Write-Host "READINESS_CASE $name close_ms=$script:closeAt original_deadline_ms=$deadline problems=$($result.Problems.Count) $(@($result.Problems) -join ' | ')"
}
Case 'pending load after old early close' 14000 14000 $true
Case 'normal three-action input path' 17000 17000 $true -Normal
Case 'missing completion despite real content' ([long]::MaxValue) 14000 $false
Case 'old completion with blank viewport and nonblank chrome' ([long]::MaxValue) ([long]::MaxValue) $false
Case 'startup completion only, then content' ([long]::MaxValue) 14000 $false
Case 'late content after the action''s completion' 15000 20000 $true
Case 'completion and content after the deadline' 31000 31000 $false
Case 'content appearing only at the deadline is not stable' 14000 29000 $false
Case 'preview not shown: editor still visible after the startup completion' ([long]::MaxValue) 14000 $false -IgnoreAccept
Case 'preview not shown: editor still visible despite a later completion' 14000 14000 $false -IgnoreAccept
Case 'association shortcut without input' ([long]::MaxValue) ([long]::MaxValue) $true -NoInput
Write-Host "READINESS_DONE checks=$script:checks failures=$script:failures"
exit $(if ($script:failures) { 1 } else { 0 })
