# The real E2E window workload on an unpacked runtime and a fresh, owned profile.
# Invoked only by benchmark.py inside its Windows Job Object. No installer,
# default user device, shell association or Start-menu mutation is performed.
param([Parameter(Mandatory)] [string] $Request)
$ErrorActionPreference = 'Stop'
$requestData = Get-Content -LiteralPath $Request -Raw | ConvertFrom-Json
$OutDir = [IO.Path]::GetFullPath($requestData.output)
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$work = $requestData.workload
$app = [IO.Path]::GetFullPath($requestData.artifacts.runtime)
$apk = [IO.Path]::GetFullPath($work.apk)
$script:PerfJobOwnsCleanup = $true
$invocation = [string]$requestData.invocation_id
$profile = Join-Path $OutDir 'profile'
$result = [ordered]@{
  schema_version = 1; invocation_id = $invocation; variant = [string]$requestData.variant
  workload_id = [string]$work.id; completed = $false; success = $false
  coverage_complete = $false; problems = @(); workload_elapsed_ms = 0.0
  events = @(); metrics = @{}; participants = @(); evidence = @{}
}

function InvokePerfFileIo([scriptblock] $Operation, [Diagnostics.Stopwatch] $Clock,
                          [double] $TimeoutSeconds = 30) {
  if (-not $Clock) { $Clock = [Diagnostics.Stopwatch]::StartNew() }
  for ($attempt = 0; $attempt -lt 5; $attempt++) {
    if ($Clock.Elapsed.TotalSeconds -ge $TimeoutSeconds) { throw 'Workload protocol exceeded its existing deadline' }
    try { $value = & $Operation }
    catch {
      $cause = $_.Exception.GetBaseException()
      $nativeCode = $cause.HResult -band 0xffff
      # Access denied can be permanent. Retry only native Windows transport
      # errors, at most four times; the final error remains a failed workload.
      if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT -or
          ($cause -isnot [IO.IOException] -and $cause -isnot [UnauthorizedAccessException]) -or
          $nativeCode -notin @(5, 32) -or $attempt -eq 4) { throw }
      $remaining = $TimeoutSeconds - $Clock.Elapsed.TotalSeconds
      if ($remaining -le 0) { throw 'Workload protocol exceeded its existing deadline' }
      $backoff = 10 * [Math]::Pow(2, $attempt)
      Start-Sleep -Milliseconds ([int][Math]::Min($backoff, [Math]::Floor($remaining * 1000)))
      continue
    }
    if ($Clock.Elapsed.TotalSeconds -ge $TimeoutSeconds) { throw 'Workload protocol exceeded its existing deadline' }
    return $value
  }
}

function ReadPerfJson([string] $Path, [Diagnostics.Stopwatch] $Clock) {
  if (-not $Clock) { $Clock = [Diagnostics.Stopwatch]::StartNew() }
  $text = InvokePerfFileIo -Clock $Clock -Operation {
    # Keep one complete file object open while allowing the peer to replace it.
    # In-place writes are denied. This is separate from post-failure diagnostics.
    $sharing = [IO.FileShare]::Read -bor [IO.FileShare]::Delete
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, $sharing)
    try {
      if ($stream.Length -gt 4MB) { throw 'JSON input exceeds the 4 MiB evidence budget' }
      $reader = [IO.StreamReader]::new($stream, [Text.UTF8Encoding]::new($false, $true), $true)
      try { $reader.ReadToEnd() } finally { $reader.Dispose() }
    } finally { $stream.Dispose() }
  }
  # Parsing errors are not retried or skipped as if no message had arrived.
  $value = $text | ConvertFrom-Json
  if ($Clock.Elapsed.TotalSeconds -ge 30) { throw 'Workload protocol exceeded its existing deadline' }
  return $value
}

function WritePerfJson([string] $Name, $Value, [Diagnostics.Stopwatch] $Clock) {
  $path = Join-Path $OutDir $Name
  $temporary = "$path.tmp"
  $text = $Value | ConvertTo-Json -Depth 20
  if ([Text.Encoding]::UTF8.GetByteCount($text) -gt 4MB) { throw 'JSON input exceeds the 4 MiB evidence budget' }
  [IO.File]::WriteAllText($temporary, $text, [Text.UTF8Encoding]::new($false))
  InvokePerfFileIo -Clock $Clock -Operation {
    if ([IO.File]::Exists($path)) {
      [IO.File]::Replace($temporary, $path, [NullString]::Value)
    } else {
      [IO.File]::Move($temporary, $path)
    }
  }
}

function MeasurementPhase([string] $Phase, $Participants = @()) {
  $deadline = [Diagnostics.Stopwatch]::StartNew()
  WritePerfJson 'phase.json' @{ invocation_id = $invocation; phase = $Phase; participants = @($Participants) } $deadline
  while ($deadline.Elapsed.TotalSeconds -lt 30) {
    $ackPath = Join-Path $OutDir 'ack.json'
    if (Test-Path -LiteralPath $ackPath) {
      $ack = ReadPerfJson $ackPath $deadline
      if ($ack.invocation_id -cne $invocation) { throw 'Measurement acknowledgement belongs to another invocation' }
      if ($ack.phase -eq $Phase) { return }
    }
    Start-Sleep -Milliseconds 10
  }
  throw "Measurement $Phase acknowledgement timed out"
}

$script:registrationSequence = 0
function RegisterPerfParticipants($Participants) {
  $script:registrationSequence++
  $deadline = [Diagnostics.Stopwatch]::StartNew()
  WritePerfJson 'participants.json' @{ invocation_id = $invocation; sequence = $script:registrationSequence;
                                      participants = @($Participants) } $deadline
  while ($deadline.Elapsed.TotalSeconds -lt 30) {
    $ackPath = Join-Path $OutDir 'participants-ack.json'
    if (Test-Path -LiteralPath $ackPath) {
      $ack = ReadPerfJson $ackPath $deadline
      if ($ack.invocation_id -cne $invocation) { throw 'Participant acknowledgement belongs to another invocation' }
      if ($ack.sequence -eq $script:registrationSequence) {
        if ($ack.verified -isnot [bool] -or -not $ack.verified) { throw 'Participant ownership was not verified' }
        return $true
      }
    }
    Start-Sleep -Milliseconds 10
  }
  throw 'Live participant registration acknowledgement timed out'
}

function DeviceParticipants([switch] $Required) {
  $path = Join-Path $profile 'Gesher\data\system\gesher-device'
  if (-not (Test-Path -LiteralPath $path)) {
    if ($Required) { throw 'The measured device has no state file in its owned profile' }
    return @()
  }
  $state = @{}
  foreach ($line in Get-Content -LiteralPath $path) {
    if ($line -match '^([^#!][^=]*)=(.*)$') { $state[$Matches[1].Trim()] = $Matches[2].Trim() }
  }
  $items = @()
  foreach ($role in @('binderd', 'servicemanager', 'system_server')) {
    $id = 0; $created = 0L
    if (-not [int]::TryParse($state[$role], [ref]$id) -or $id -le 0 -or
        -not [long]::TryParse($state["$role.start"], [ref]$created) -or $created -le 0) {
      throw "Missing device process identity: $role"
    }
    $process = [Diagnostics.Process]::GetProcessById($id)
    try {
      if ($process.StartTime.ToFileTimeUtc() -ne $created) { throw "Device process identity changed: $role" }
      $items += @{ pid = $id; created_100ns = $created; role = $role }
    } finally { $process.Dispose() }
  }
  return $items
}

function WorkloadParticipants {
  $items = @(DeviceParticipants -Required)
  # WebView2 can use brokered process creation. Check its actual fresh-profile
  # processes too; the parent rejects any that escaped its job accounting.
  foreach ($entry in @(Get-CimInstance Win32_Process -Filter "Name='msedgewebview2.exe'")) {
    if (-not $entry.CommandLine) { throw 'Cannot establish WebView2 process coverage: command line unavailable' }
    if ($entry.CommandLine.IndexOf($profile, [StringComparison]::OrdinalIgnoreCase) -lt 0) { continue }
    $process = [Diagnostics.Process]::GetProcessById([int]$entry.ProcessId)
    try {
      $items += @{ pid = $process.Id; created_100ns = $process.StartTime.ToFileTimeUtc(); role = 'webview2' }
    } finally { $process.Dispose() }
  }
  return $items
}

$phaseStarted = $false
try {
  if ($env:OS -ne 'Windows_NT' -or $invocation -cnotmatch '^[0-9a-f]{32}$' -or
      $env:GESHER_PERF_ATTEMPT -cne $OutDir) { throw 'Invalid owned Windows performance invocation' }
  if (Test-Path -LiteralPath $profile) { throw 'Performance profile must be fresh; refusing existing data/device state' }
  New-Item -ItemType Directory -Path $profile | Out-Null
  # All Gesher package/device state is rooted here by the maintained launcher.
  # An inherited namespace/log override must never reconnect to another device.
  foreach ($name in @('GESHER_BINDER', 'GESHER_BINDER_UID', 'GESHER_LOGS', 'GESHER_LAUNCH_DETACH',
                       'GESHER_APP_METHOD_TRACE', 'GESHER_DISPLAY', 'GESHER_SURFACE_TRACE_INVALIDATION',
                       'GESHER_WINDOW_DEBUG')) {
    Remove-Item "Env:$name" -ErrorAction SilentlyContinue
  }
  $env:LOCALAPPDATA = $profile
  $env:GESHER_SHORTCUTS = '0'
  foreach ($name in @('gesher-app.cmd', 'gesher-app.ps1', 'dalvikvm.exe', 'gesher-app.jar', 'framework.jar')) {
    if (-not (Test-Path -LiteralPath (Join-Path $app $name) -PathType Leaf)) { throw "Incomplete runtime: $name" }
  }
  $manifestSha = (Get-FileHash -LiteralPath $work.manifest -Algorithm SHA256).Hash.ToLowerInvariant()
  $apkSha = (Get-FileHash -LiteralPath $apk -Algorithm SHA256).Hash.ToLowerInvariant()
  if ($manifestSha -cne $requestData.artifact_identities.manifest.sha256 -or
      $apkSha -cne $requestData.artifact_identities.apk.sha256) { throw 'APK or manifest changed after freezing' }
  $manifest = Get-Content -LiteralPath $work.manifest -Raw | ConvertFrom-Json
  $selected = @($manifest.apps | Where-Object { $_.package -ceq $work.package })
  if ($selected.Count -ne 1 -or $selected[0].sha256 -cne $apkSha -or -not $selected[0].actions) {
    throw 'The workload needs exactly one pinned application with input actions'
  }
  $a = $selected[0]
  if ($a.trace) { throw 'Method-traced workloads need a separate instrumented scenario' }
  if ($a.display) { $env:GESHER_DISPLAY = [string]$a.display }
  . (Join-Path $root 'tests\windows\graphics\WindowChecks.ps1')
  Add-Type -AssemblyName System.Drawing
  . (Join-Path $root 'tests\e2e\window-workload.ps1')
  $script:PerfParticipantCapture = { WorkloadParticipants }
  $script:PerfRegisterParticipants = { param($Participants) RegisterPerfParticipants $Participants }
  # Preparation is before the parent's start snapshot and the measured handshake.
  $install = & (Join-Path $app 'gesher-app.cmd') install $apk 2>&1 | Out-String
  [IO.File]::WriteAllText((Join-Path $OutDir 'install.log'), $install, [Text.UTF8Encoding]::new($false))
  if ($LASTEXITCODE -ne 0 -or $install -notmatch "installed $([regex]::Escape($work.package))|$([regex]::Escape($work.package)) is installed") {
    throw "Isolated APK installation failed: $(FirstProblem $install)"
  }
  if ($work.scenario -eq 'cold-device') {
    & (Join-Path $app 'gesher-app.cmd') shutdown 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0 -or @(DeviceParticipants).Count) { throw 'Owned device did not shut down before cold-device measurement' }
    $startParticipants = @()
  } elseif ($work.scenario -eq 'warm-device') {
    $startParticipants = @(DeviceParticipants -Required)
  } else { throw 'Unsupported device scenario' }
  MeasurementPhase 'start' $startParticipants
  $phaseStarted = $true
  $observed = RunWindow $a "`"$app\gesher-app.cmd`" `"$apk`" --hidden-window" 'launch' -Measure
  MeasurementPhase 'stop'
  $phaseStarted = $false
  $result.problems += @($observed.Problems)
  $log = ReadText (Join-Path $OutDir "$($work.package).launch.log")
  $appLogPath = AppLogPath $work.package
  if (Test-Path -LiteralPath $appLogPath) {
    Copy-Item -LiteralPath $appLogPath -Destination (Join-Path $OutDir 'application.log')
    $log += ReadText $appLogPath
  }
  foreach ($expected in @($a.expectLog | Where-Object { $_ })) {
    if (-not $log.Contains($expected)) { $result.problems += "The actual log lacks '$expected'" }
  }
  if ($work.abi -eq 'arm64-v8a' -and -not $log.Contains('is arm64-v8a: translated')) {
    $result.problems += 'No observed translated ARM64 load for the requested workload ABI'
  }
  if ($work.abi -eq 'x86_64' -and $log.Contains('is arm64-v8a: translated')) {
    $result.problems += 'Translated ARM64 appeared in the declared x64 workload'
  }
  $performance = $observed.Performance
  if ($null -eq $performance) { throw 'Missing E2E timing evidence' }
  $result.events = @($performance.Events)
  $events = @{}
  foreach ($event in $result.events) { $events[$event.name] = [double]$event.at_ms }
  foreach ($name in @('command_start', 'first_frame_observed', 'first_input', 'first_input_change_observed', 'completed')) {
    if (-not $events.ContainsKey($name)) { throw "Missing E2E boundary: $name" }
  }
  $result.workload_elapsed_ms = $events.completed
  $result.metrics = @{
    command_to_first_frame_observed_ms = $events.first_frame_observed - $events.command_start
    first_input_to_change_observed_ms = $events.first_input_change_observed - $events.first_input
    workload_ms = $events.completed
  }
  $byIdentity = @{}
  foreach ($participant in $performance.Participants) { $byIdentity["$($participant.pid)/$($participant.created_100ns)"] = $participant }
  $result.participants = @($byIdentity.Values)
  $deviceRoles = @($result.participants | Where-Object { $_.role -in @('binderd', 'servicemanager', 'system_server') } |
    ForEach-Object { $_.role } | Sort-Object -Unique)
  $result.coverage_complete = $deviceRoles.Count -eq 3
  $result.evidence = @{
    package = $work.package; scenario = $work.scenario; abi = $work.abi
    abi_evidence = $(if ($work.abi -eq 'arm64-v8a') { 'observed translated-library log' } else { 'x64 runtime workload; no translated-library log; selected native ABI not independently queried' })
    manifest_sha256 = $manifestSha; apk_sha256 = $apkSha
    runtime_sha256 = $requestData.artifact_identities.runtime.sha256
    first_frame_verified = $performance.FirstFrameVerified
    nonblank_verified = $performance.NonblankVerified; input_change_verified = $performance.InputVerified
    lifecycle_verified = $performance.LifecycleVerified; fresh_profile = $true
    owned_device_verified = $result.coverage_complete
    timing_boundary = 'Observer upper bounds: 500ms first-frame polling, existing 3s startup and 2s/input delays, then 1s settling captures; not exact GPU presentation.'
    cache_state = 'Fresh application profile; cold/warm applies to the device server, not the OS/file cache.'
  }
  $result.completed = $true
  $result.success = $result.problems.Count -eq 0 -and $result.coverage_complete
} catch {
  $result.problems += $_.Exception.Message
} finally {
  if ($phaseStarted) {
    try { MeasurementPhase 'stop' } catch { $result.problems += $_.Exception.Message }
  }
  WritePerfJson 'result.json' $result
  # The parent kills only its own job. Never call a default-profile shutdown or
  # PID-based fallback from here, including after setup/measurement failures.
}
exit $(if ($result.success) { 0 } else { 1 })
