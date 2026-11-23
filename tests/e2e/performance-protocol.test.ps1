# Execute the actual adapter mailbox functions without launching an application.
param(
  [ValidateSet('SelfTest', 'Read', 'Write')] [string] $Action = 'SelfTest',
  [string] $Path,
  [string] $ValueFile
)
$ErrorActionPreference = 'Stop'
$source = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'tools/perf/e2e-workload.ps1'
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($source, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw ($errors -join "`n") }
foreach ($name in @('InvokePerfFileIo', 'ReadPerfJson', 'WritePerfJson', 'MeasurementPhase', 'RegisterPerfParticipants')) {
  $functions = @($ast.FindAll({ param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] }, $true) |
    Where-Object Name -eq $name)
  if ($functions.Count -ne 1) { throw "Missing actual adapter function: $name" }
  Invoke-Expression $functions[0].Extent.Text
}
if ($Action -ne 'SelfTest') {
  $OutDir = [IO.Path]::GetDirectoryName($Path)
  if ($Action -eq 'Read') {
    ReadPerfJson $Path ([Diagnostics.Stopwatch]::StartNew()) | ConvertTo-Json -Depth 20 -Compress
  } else {
    $value = [IO.File]::ReadAllText($ValueFile) | ConvertFrom-Json
    WritePerfJson ([IO.Path]::GetFileName($Path)) $value ([Diagnostics.Stopwatch]::StartNew())
  }
  exit 0
}

$script:checks = 0; $script:failures = 0
function Check([bool] $Condition, [string] $Name) {
  $script:checks++
  if (-not $Condition) { $script:failures++; Write-Host "PERF_PROTOCOL_FAIL $Name" }
}
function Rejected([scriptblock] $Operation, [string] $Name) {
  try { & $Operation | Out-Null }
  catch { Check $true $Name; return $_.Exception.GetBaseException() }
  Check $false $Name
  return $null
}
$OutDir = Join-Path ([IO.Path]::GetTempPath()) ('gesher-perf-protocol-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $OutDir | Out-Null
$invocation = 'a' * 32
$script:registrationSequence = 0
try {
  $path = Join-Path $OutDir 'phase.json'
  WritePerfJson 'phase.json' @{ phase = 'start' }
  Check ((ReadPerfJson $path ([Diagnostics.Stopwatch]::StartNew())).phase -eq 'start') 'owned JSON roundtrip'
  $sharing = [IO.FileShare]::Read -bor [IO.FileShare]::Delete
  $held = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, $sharing)
  try {
    WritePerfJson 'phase.json' @{ phase = 'stop' }
    $reader = [IO.StreamReader]::new($held)
    try { $old = $reader.ReadToEnd() | ConvertFrom-Json } finally { $reader.Dispose() }
    Check ($old.phase -eq 'start') 'held reader retains complete old snapshot'
    Check ((ReadPerfJson $path ([Diagnostics.Stopwatch]::StartNew())).phase -eq 'stop') 'new reader sees complete new snapshot'
  } finally { $held.Dispose() }

  [IO.File]::WriteAllText($path, '{malformed')
  $null = Rejected { ReadPerfJson $path ([Diagnostics.Stopwatch]::StartNew()) } 'malformed JSON remains a failure'
  [IO.File]::WriteAllBytes($path, [byte[]]@(255))
  $null = Rejected { ReadPerfJson $path ([Diagnostics.Stopwatch]::StartNew()) } 'invalid UTF8 remains a failure'
  [IO.File]::WriteAllText($path, ('x' * (4MB + 1)))
  $null = Rejected { ReadPerfJson $path ([Diagnostics.Stopwatch]::StartNew()) } 'oversized snapshot remains a failure'
  $script:called = 0
  $null = Rejected { InvokePerfFileIo -TimeoutSeconds 0 -Clock ([Diagnostics.Stopwatch]::StartNew()) -Operation {
    $script:called++; throw 'must not execute expired operation'
  } } 'expired deadline rejected'
  Check ($script:called -eq 0) 'expired deadline performs no I/O'
  $script:called = 0
  $null = Rejected { InvokePerfFileIo -Operation { $script:called++; throw [FormatException]::new('semantic failure') } } 'semantic failure rejected'
  Check ($script:called -eq 1) 'semantic failure never retried'

  WritePerfJson 'ack.json' @{ invocation_id = ('b' * 32); phase = 'start' }
  $null = Rejected { MeasurementPhase 'start' } 'foreign phase acknowledgement rejected'
  WritePerfJson 'ack.json' @{ invocation_id = $invocation; phase = 'start' }
  MeasurementPhase 'start'
  Check $true 'matching phase acknowledgement accepted'
  WritePerfJson 'participants-ack.json' @{ invocation_id = ('b' * 32); sequence = 1; verified = $true }
  $null = Rejected { RegisterPerfParticipants @(@{ pid = 1; created_100ns = 1 }) } 'foreign registration acknowledgement rejected'
  WritePerfJson 'participants-ack.json' @{ invocation_id = $invocation; sequence = 2; verified = $false }
  $null = Rejected { RegisterPerfParticipants @(@{ pid = 1; created_100ns = 1 }) } 'unverified ownership acknowledgement rejected'
  WritePerfJson 'participants-ack.json' @{ invocation_id = $invocation; sequence = 3; verified = $true }
  Check (RegisterPerfParticipants @(@{ pid = 1; created_100ns = 1 })) 'matching verified registration acknowledgement accepted'

  if ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT) {
    WritePerfJson 'phase.json' @{ phase = 'start' }
    $script:held = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $script:ioSleeps = 0; $script:release = $false
    function Start-Sleep([int] $Milliseconds) {
      $script:ioSleeps++
      if ($script:release) { $script:held.Dispose(); $script:release = $false }
    }
    try {
      $conflict = Rejected { WritePerfJson 'phase.json' @{ phase = 'stop' } } 'persistent non-sharing reader rejected'
      Check (($conflict.HResult -band 0xffff) -in @(5, 32)) 'persistent native error retained'
      Check ($script:ioSleeps -eq 4) 'persistent replacement uses exactly five attempts'
      Check ((ReadPerfJson $path ([Diagnostics.Stopwatch]::StartNew())).phase -eq 'start') 'failed replacement keeps old message'
      $script:ioSleeps = 0; $script:release = $true
      WritePerfJson 'phase.json' @{ phase = 'stop' }
      Check ($script:ioSleeps -eq 1) 'actual conflict recovers after first holder release'
      Check ((ReadPerfJson $path ([Diagnostics.Stopwatch]::StartNew())).phase -eq 'stop') 'recovered replacement publishes full message'
    } finally {
      $script:held.Dispose()
      Remove-Item Function:\Start-Sleep
    }
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class PerfProtocolDeleteHandle {
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
  public static extern IntPtr CreateFileW(string path, uint access, uint share, IntPtr security, uint creation, uint flags, IntPtr template);
  [DllImport("kernel32.dll")]
  public static extern bool CloseHandle(IntPtr handle);
}
'@
    $delete = [PerfProtocolDeleteHandle]::CreateFileW($path, 0x10000, 7, [IntPtr]::Zero, 3, 0x80, [IntPtr]::Zero)
    if ($delete.ToInt64() -eq -1) { throw 'Native delete-handle fixture did not open' }
    try {
      $null = Rejected { [IO.File]::ReadAllText($path) } 'ordinary reader conflicts with live DELETE handle'
      Check ((ReadPerfJson $path ([Diagnostics.Stopwatch]::StartNew())).phase -eq 'stop') 'actual adapter reader shares live DELETE handle'
    } finally { [void][PerfProtocolDeleteHandle]::CloseHandle($delete) }
    Write-Host 'PERF_PROTOCOL_NATIVE shared_delete=true held_snapshot=true persistent_denial=true'
  } else {
    Write-Host 'PERF_PROTOCOL_NATIVE skipped: requires Windows handle sharing'
  }
  Write-Host "PERF_PROTOCOL checks=$script:checks failures=$script:failures"
  if ($script:failures) { throw 'Actual adapter protocol contracts failed' }
} finally {
  Remove-Item -LiteralPath $OutDir -Recurse -Force
}
