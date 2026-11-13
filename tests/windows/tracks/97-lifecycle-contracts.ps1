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
