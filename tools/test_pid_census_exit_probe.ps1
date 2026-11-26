$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Add-Type -Path "$repo/tools/windows4/TsfJobSupervisor.cs"
Add-Type -Path "$repo/tests/windows/helpers/PidCensusExitProbe.cs"
$count = 0
function Require([bool]$okay, [string]$message) {
  if (-not $okay) { throw $message }
  $script:count++
}
foreach ($path in @('tests/windows/helpers/geocoder-compile.ps1', 'tests/windows/helpers/pid-census-exit-probe.ps1')) {
  $tokens = $null; $errors = $null
  $null = [Management.Automation.Language.Parser]::ParseFile((Join-Path $repo $path), [ref]$tokens, [ref]$errors)
  Require ($errors.Count -eq 0) "Parse failed: $path"
}
$previous = $env:GITHUB_ACTIONS
try {
  $env:GITHUB_ACTIONS = 'false'
  $caught = ''
  try { [void][PidCensusExitProbe]::Run([TsfJobSupervisor], 'cmd.exe', 0) } catch { $caught = $_.Exception.ToString() }
  Require ($caught.Contains('CI only')) 'Non-CI call reached native boundary'
  $env:GITHUB_ACTIONS = 'true'
  $caught = ''
  try { [void][PidCensusExitProbe]::Run($null, 'cmd.exe', 0) } catch { $caught = $_.Exception.ToString() }
  Require ($caught.Contains('Actual controller type missing')) 'Null controller reached native boundary'
  $caught = ''
  try { [void][PidCensusExitProbe]::Run([string], 'cmd.exe', 0) } catch { $caught = $_.Exception.ToString() }
  Require ($caught.Contains('Actual controller type missing')) 'Wrong controller reached native boundary'
} finally { $env:GITHUB_ACTIONS = $previous }
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Collections.Generic;
public static class PidProbeHostControls {
  public static List<uint> Complete(IntPtr ignored) { return new List<uint> { 42 }; }
  public static List<uint> Incomplete(IntPtr ignored) { throw new IOException("Incomplete job PID census"); }
  public static List<uint> OtherIo(IntPtr ignored) { throw new IOException("Other failure sentinel"); }
  public static List<uint> WrongType(IntPtr ignored) { throw new InvalidOperationException("Incomplete job PID census"); }
}
'@
$invoke = [PidCensusExitProbe].GetMethod('ActualController', [Reflection.BindingFlags]'NonPublic,Static')
foreach ($name in @('Complete', 'Incomplete', 'OtherIo', 'WrongType')) {
  $observation = [PidCensusExitProbe+Observation]::new()
  $method = [PidProbeHostControls].GetMethod($name)
  $caught = ''
  try { [void]$invoke.Invoke($null, @($method, [IntPtr]::Zero, $observation)) } catch { $caught = $_.Exception.ToString() }
  Require ($observation.ControllerCalls -eq 1) "Invocation not counted: $name"
  if ($name -ceq 'Incomplete') {
    Require ($observation.ControllerIncomplete -eq 1 -and $caught -ceq '') 'Exact incomplete diagnostic not retained'
  } elseif ($name -ceq 'Complete') {
    Require ($observation.ControllerIncomplete -eq 0 -and $caught -ceq '') 'Complete result misclassified'
  } else {
    Require ($observation.ControllerIncomplete -eq 0 -and $caught -cne '') "Unexpected failure swallowed: $name"
  }
}
Write-Host "PID_CENSUS_HOST_CHECKS_PASS checks=$count native_windows_execution=False"
