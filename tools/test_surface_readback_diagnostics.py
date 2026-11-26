#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Execute the complete SurfaceRun with real POSIX child/file lifetimes.

Only Win32 window observations/readback are typed controlled boundaries. This
host-only test preserves the actual PowerShell capture/poll/cleanup code and its
original 500-second/120-second limits; it does not qualify Windows pixels.
"""
from pathlib import Path
import argparse,hashlib,json,os,shutil,subprocess
ROOT=Path(__file__).resolve().parents[1]
TREE=ROOT
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--pwsh', type=Path, default=shutil.which('pwsh'))
parser.add_argument('--out', type=Path, required=True, help='New directory retaining every real child/log/trace receipt')
parser.add_argument('--source-ref', help='Execute the complete original SurfaceRun as an observability negative')
args=parser.parse_args()
if os.name != 'posix' or not args.pwsh:
    parser.error('This explicit POSIX-child host boundary requires PowerShell 7; use Windows CI for real window readback')
OUT=args.out.resolve();OUT.mkdir(parents=True,exist_ok=False)
SOURCE=TREE/'tests/windows/tracks/52-surfaces.ps1'
if args.source_ref:
    SOURCE=OUT/'original-52-surfaces.ps1'
    SOURCE.write_bytes(subprocess.check_output(['git','show',args.source_ref+':tests/windows/tracks/52-surfaces.ps1'],cwd=TREE))
bin=OUT/'bin';bin.mkdir(exist_ok=True)
child=bin/'cmd.exe'
child.write_text('''#!/usr/bin/env python3
import os,pathlib,time
case=os.environ['SURFACE_DIAG_CASE'];p=pathlib.Path(os.environ['SURFACE_DIAG_APP_LOG'])
with p.open('a') as f:
 def out(s): f.write(s+'\\n'); f.flush()
 out('controlled real wrapper pid '+str(os.getpid()))
 if case in ['valid','null','big-log','window-observation-error','io-failure']:
  if case=='big-log': out('a'*200000)
  out('first frame drawn (1 presented), fixture, hwnd 0x1234')
  out('scaling: phase 1 ready')
  time.sleep(2.1)
  out('scaling: done')
 elif case=='done-before-capture':
  out('first frame drawn (1 presented), fixture, hwnd 0x1234')
  out('scaling: phase 1 ready');out('scaling: done')
 elif case=='no-window':
  out('scaling: phase 1 ready');time.sleep(.3)
 elif case=='stale-log': out('scaling: done')
 elif case=='zero-phase': pass
 else: raise AssertionError(case)
''');child.chmod(0o755)
script=OUT/'controls.ps1'
script.write_text(r'''param([string] $Source, [string] $Output)
$ErrorActionPreference = 'Stop'
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($Source, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw 'Actual SurfaceRun source does not parse' }
$function = @($ast.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'SurfaceRun' }, $true))
if ($function.Count -ne 1) { throw 'Exactly one real SurfaceRun is required' }
Invoke-Expression $function[0].Extent.Text
Add-Type @'
using System;
public static class GesherWin32 {
  public static int Captures;
  public static string Mode;
  public static bool IsWindow(IntPtr hwnd) {
    if (Mode == "window-observation-error") throw new InvalidOperationException("controlled observation boundary");
    return hwnd == (IntPtr)0x1234;
  }
  public static uint ProcessId(IntPtr hwnd) { return 4321; }
  public static int[] ClientSize(IntPtr hwnd) { return new int[] { 2, 2 }; }
  public static int[] Capture(IntPtr hwnd) {
    Captures++;
    if (Mode == "null") return null;
    return new int[] { 2, 2, 0xff0000, 0x00ff00, 0x0000ff, 0xffffff };
  }
}
'@
$checks = 0
function Require([bool] $Condition, [string] $Message) {
  $script:checks++
  if (-not $Condition) { throw $Message }
}
$Dir = $Output
$dvmSurfaces = @()
$rows = [Collections.Generic.List[object]]::new()
foreach ($case in @('valid','null','done-before-capture','no-window','stale-log','zero-phase','big-log','window-observation-error','io-failure')) {
  $folder = Join-Path $Output $case
  [void][IO.Directory]::CreateDirectory($folder)
  $appLog = Join-Path $folder 'app.log'
  $old = if ($case -eq 'stale-log') { "first frame drawn (1 presented), fixture, hwnd 0xdead`nscaling: phase 1 ready`n" } else { '' }
  [IO.File]::WriteAllText($appLog, $old)
  $env:RUNNER_TEMP = $folder
  $env:SURFACE_DIAG_APP_LOG = $appLog
  $env:SURFACE_DIAG_CASE = $case
  [GesherWin32]::Captures = 0; [GesherWin32]::Mode = $case
  if ($case -eq 'io-failure') { [IO.File]::WriteAllText((Join-Path $folder 'gesher-surface-evidence'), 'owned obstruction') }
  $phases = if ($case -eq 'zero-phase') { 0 } else { 1 }
  $result = SurfaceRun -Arguments @('fixture') -Tag $case -AppLogs @($appLog) -Prefix 'scaling' -Phases $phases
  Require ($result.Exit -eq 0) "$case changed actual process exit"
  $expectedCaptures = if ($case -in @('valid','null','big-log','window-observation-error','io-failure')) { 1 } else { 0 }
  Require ([GesherWin32]::Captures -eq $expectedCaptures) "$case changed original capture decisions/count"
  Require ($result.Shots.Count -eq $expectedCaptures) "$case changed original phase advancement"
  if ($case -eq 'null') { Require ($null -eq $result.Shots[1]) 'Null capture was replaced/retried' }
  elseif ($expectedCaptures -eq 1) { Require ($result.Shots[1].Length -eq 6 -and $result.Shots[1][2] -eq 0xff0000) 'Captured pixels changed' }
  if ($case -eq 'io-failure') {
    Require (Test-Path (Join-Path $folder 'gesher-surface-evidence') -PathType Leaf) 'Diagnostic failure was hidden by deleting the obstruction'
    $rows.Add(@{Case=$case;Captured=$expectedCaptures;DiagnosticFailurePreservedResult=$true})
    continue
  }
  Require (Test-Path (Join-Path $folder 'gesher-surface-evidence') -PathType Container) 'Missing actual SurfaceRun diagnostic artifact'
  $files = @(Get-ChildItem (Join-Path $folder 'gesher-surface-evidence') -Filter '*.json')
  Require ($files.Count -eq 1) 'Exactly one owned trace is required'
  $trace = Get-Content -Raw $files[0].FullName | ConvertFrom-Json
  Require ($trace.AppLogStartCharacters.$appLog -eq $old.Length) 'Saved character offset differs from actual reader offset'
  Require ($trace.Events.Count -le 128 -and $trace.DroppedEvents -eq 0) 'Trace bound changed'
  $attempts = @($trace.Events | Where-Object Kind -eq 'capture-attempt')
  $results = @($trace.Events | Where-Object Kind -eq 'capture-result')
  Require ($attempts.Count -eq $expectedCaptures -and $results.Count -eq $expectedCaptures) 'Trace conflated absent and attempted capture'
  Require ($result.Text.Contains("controlled real wrapper pid $($trace.WrapperPid)")) 'Trace wrapper identity is not the real owned process'
  $logPath = [IO.Path]::ChangeExtension($files[0].FullName, '.log')
  $retained = [IO.File]::ReadAllText($logPath)
  Require ($retained.Length -le 131072) 'Retained log exceeded its declared character bound'
  if ($case -eq 'big-log') {
    Require ($trace.LogCharacters -gt 200000 -and $retained.Contains('[combined log middle omitted]')) 'Large real log was not explicitly bounded'
  } else { Require ($retained -ceq $result.Text) 'Small observed log was changed' }
  if ($expectedCaptures -eq 1) {
    Require ($trace.PollBreakReason -eq 'phase-limit') 'A capture changed the loop stopping condition'
    Require ($results[0].CaptureReturnedNull -eq ($case -eq 'null')) 'Actual null result was mislabeled'
    if ($case -eq 'window-observation-error') { Require ([bool]$results[0].WindowObservationError) 'Observation error was lost' }
    else { Require ($attempts[0].Hwnd -eq '0x1234' -and $attempts[0].WindowOwnerPid -eq 4321 -and $attempts[0].ClientWidth -eq 2) 'HWND boundary facts were lost' }
  } elseif ($case -in @('done-before-capture','stale-log')) {
    Require ($trace.PollBreakReason -eq 'done-marker') 'Done-before-capture ordering changed'
  } elseif ($case -eq 'no-window') { Require ($trace.PollBreakReason -eq 'wrapper-exited') 'Actual process-exit break changed' }
  else { Require ($trace.PollBreakReason -eq 'phase-limit') 'Zero-phase process control changed' }
  if ($case -eq 'stale-log') { Require ($result.Hwnd -eq [IntPtr]::Zero) 'Old appended log selected a previous HWND' }
  $rows.Add(@{Case=$case;Captured=$expectedCaptures;Trace=$files[0].FullName;BreakReason=$trace.PollBreakReason})
}
# Execute the exact source observer assignment on 140 calls; it must stay purely observational.
$assignment = @($function[0].FindAll({param($node) $node -is [Management.Automation.Language.AssignmentStatementAst] -and $node.Left.Extent.Text -eq '$observe'}, $true))
Require ($assignment.Count -eq 1) 'One exact bounded observer is required'
$trace = [ordered]@{Events=[Collections.Generic.List[object]]::new();DroppedEvents=0}
$traceClock = [Diagnostics.Stopwatch]::StartNew()
Invoke-Expression $assignment[0].Extent.Text
[GesherWin32]::Captures = 0
1..140 | ForEach-Object { & $observe 'phase-marker' $_ ([IntPtr]::Zero) 0 }
Require ($trace.Events.Count -eq 128 -and $trace.DroppedEvents -eq 12 -and [GesherWin32]::Captures -eq 0) 'Observer cap changes capture or retains unbounded state'
$rows | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $Output 'outcomes.json')
Write-Output "Actual SurfaceRun controlled-process diagnostics: $checks checks PASS; Win32 readback remains a typed host boundary"
''')
env=os.environ.copy();env['PATH']=str(bin)+os.pathsep+env['PATH']
command=[str(args.pwsh),'-NoProfile','-File',str(script),'-Source',str(SOURCE),'-Output',str(OUT)]
r=subprocess.run(command,env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=120)
(OUT/'run.log').write_text(r.stdout)
receipt={'command':command,'exit':r.returncode,'source_sha256':hashlib.sha256(SOURCE.read_bytes()).hexdigest(),'driver_sha256':hashlib.sha256(script.read_bytes()).hexdigest(),'child_sha256':hashlib.sha256(child.read_bytes()).hexdigest(),'log_sha256':hashlib.sha256(r.stdout.encode()).hexdigest(),'boundary':'unchanged complete PowerShell SurfaceRun; real POSIX child process/appended file I/O; typed Win32 observation/capture boundary; no Windows/ART evidence'}
(OUT/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
print(r.stdout);r.check_returncode()
