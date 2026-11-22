# Actual Windows ART/Handler/Parcel and native descriptor/callback lifetime gates.
# RunTimed, FixesRun, Check and $dvmFixes are initialized by the earlier tracks.
foreach ($fixture in @(
  @{ Main = 'NetworkBuilderContractTest'; Markers = @('NetworkBuilderContractTest all passed:', 'parcel=true');
     Label = 'public SDK network builders and native Parcel' },
  @{ Main = 'NetworkBuilderInternalContractTest'; Markers = @('NetworkBuilderInternalContractTest all passed:', 'parcel=true');
     Label = 'network transport/specifier and enterprise data contracts' },
  @{ Main = 'ConnectivityCallbackContractTest'; Markers = @('ConnectivityCallbackContractTest all passed:', 'parcel=true');
     Label = 'network callback identity, timeout retirement, Binder and Handler' },
  @{ Main = 'WebMessagePortContractTest'; Markers = @('WebMessagePort contracts:', ' checks, 0 failures');
     Label = 'message port ownership, queued delivery and real Handler lifecycle' },
  @{ Main = 'CodecLifecycleContractTest'; Markers = @('CodecLifecycleContractTest:', ' checks passed; native=true');
     Label = 'MediaCodec retained operations and bounded release interruption' }
)) {
  $lifecycleOut = FixesRun ($dvmFixes + ('gesher.services.' + $fixture.Main)) 90
  $lifecycleCode = $LASTEXITCODE
  Check ('lifecycle: ' + $fixture.Label) {
    $global:LASTEXITCODE = $lifecycleCode
    $lifecycleOut
  } $fixture.Markers
}

foreach ($fixture in @(
  @{ Exe = 'timerfd_windows_test.exe'; Markers = @('timerfd-host:', ' assertions, 0 failures');
     Label = 'actual timer backend, alias ownership and descriptor reuse' },
  @{ Exe = 'file_flags_ofd_test.exe'; Markers = @('file_flags_ofd_test:', ' assertions, 0 failures');
     Label = 'file status flags, OFD sharing across dup, and independent FD_CLOEXEC' },
  @{ Exe = 'stdio_ofd_test.exe'; Markers = @('stdio OFD contracts:', ' checks, 0 failures');
     Label = 'shared stdio buffers, append flags, descriptor ownership and concurrent close' },
  @{ Exe = 'pipe_kernel_test.exe'; Markers = @('pipe kernel contracts:', ' checks, 0 failures');
     Label = 'kernel pipe admission, cancellation, aliases and truthful unsupported operations' },
  @{ Exe = 'pipe_native_process_test.exe'; Markers = @('pipe native process contracts:', ' checks, 0 failures');
     Label = 'native peer lifetime, shared process admission and external sync/async pipe I/O' },
  @{ Exe = 'revocable_file_test.exe'; Markers = @('revocable Windows contracts:', ' checks, 0 failures');
     Label = 'revocable descriptor aliases, in-flight I/O barrier and bounded native resources' },
  @{ Exe = 'ofd_spawn_test.exe'; Markers = @('OFD Windows spawn:', ' checks, 0 failures');
     Label = 'actual importer discovery, shared file descriptions and native PE interoperability' },
  @{ Exe = 'media_input_surface_callback_test.exe'; Markers = @('input surface callback contracts:', ' checks, 0 failures');
     Label = 'actual QueueSurface callback admission and quiescent retirement' }
)) {
  $lifecycleOut = RunTimed "$Dir/$($fixture.Exe)" @() 90
  $lifecycleCode = $LASTEXITCODE
  Check ('lifecycle: ' + $fixture.Label) {
    $global:LASTEXITCODE = $lifecycleCode
    $lifecycleOut
  } $fixture.Markers
}

$stdioExitPath = Join-Path ([IO.Path]::GetTempPath()) "gesher-stdio-exit-$PID.txt"
try {
  $stdioExitOut = RunTimed "$Dir/stdio_ofd_test.exe" @('--exit-output', $stdioExitPath) 30
  $stdioExitCode = $LASTEXITCODE
  Check 'stdio: flush after application exit handlers' {
    $global:LASTEXITCODE = $stdioExitCode
    $stdioExitOut
    if ((Test-Path $stdioExitPath) -and [IO.File]::ReadAllText($stdioExitPath) -eq 'earlylate') {
      'stdio exit-handler output: earlylate'
    }
  } @('stdio exit-handler output: earlylate')
} finally {
  Remove-Item $stdioExitPath -Force -ErrorAction SilentlyContinue
}

$stdioTlsOut = RunTimed "$Dir/stdio_ofd_test.exe" @('--tls-exit') 30
$stdioTlsCode = $LASTEXITCODE
Check 'stdio: concurrent output and pthread TLS destruction' {
  $global:LASTEXITCODE = $stdioTlsCode
  $stdioTlsOut
} @('stdio TLS exit: all passed')

$fontScope = $env:GESHER_FONT_REGRESSION_SCOPE
# Unselected/manual execution retains the strict default. CI selects the
# supported scope explicitly; an unknown selection never drops an assertion.
if (-not $fontScope) { $fontScope = 'functional-and-strict' }
if ($fontScope -cnotin @('functional', 'functional-and-strict')) {
  throw "Unknown font regression scope: $fontScope"
}
Check 'environment: font values, borrowed storage, read callback and concurrent snapshots' {
  & "$PSScriptRoot/../run-environment-owner-regression.ps1" -Fixture "$Dir/font_environment_test.exe" -Scope functional
} @('font environment functional regression: 5 cases passed; held_writer_callback_qualified=0 crt_owner_qualified=0')
if ($fontScope -ceq 'functional-and-strict') {
  Check 'environment: strict held CRT owner and concurrent callback completion' {
    & "$PSScriptRoot/../run-environment-owner-regression.ps1" -Fixture "$Dir/font_environment_test.exe" -Scope strict
  } @('font environment regression: 5 cases passed')
} else {
  Write-Host 'ENVIRONMENT_HOST_STRICT_WCT_NOT_RUN selected_scope=functional held_writer_callback_qualified=0 crt_owner_qualified=0 historical_Windows2025_WCT_qualified=0'
}
