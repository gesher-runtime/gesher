# Graphics (docs/graphics.md): Skia, hwui, the Win32 window and input.
# Dot-sourced by run-tests.ps1: Check, RunDex, NoDrive, $Dir, $root, $bcp and
# $failed are its.

# Skia's CPU raster backend: shapes, antialiasing, FreeType text, codecs.
Check 'skia: CPU raster, FreeType text, PNG/JPEG/WebP codecs' {
  & "$Dir/skia_probe.exe" 'C:/Windows/Fonts/arial.ttf' (Join-Path $env:RUNNER_TEMP 'skia_probe.png')
} @('all passed')

# The Win32 window (compat/hwui/window): Skia draws into its back buffer and
# PrintWindow reads it back off-screen; posted input comes out of its queue;
# Win32Surface (the ANativeWindow over it) presents RGBA frames.
Check 'window: Win32 window, PrintWindow read-back, input queue, Win32Surface' {
  & "$Dir/window_probe.exe" (Join-Path $env:RUNNER_TEMP 'window_probe.png')
} @('all passed')

# Runs a class of graphics-tests.dex (tests/dex-graphics) with framework.jar
# and gesher-app.jar on the boot class path, with a time limit (a render
# thread that goes wrong tends to wait forever); passes when it exits 0 and
# prints "<Main> all passed".
. "$PSScriptRoot/../helpers/ProcessReaderHandles.ps1"
function GraphicsDex([string] $Name, [string] $Main, [string[]] $Arguments = @(), [int] $Seconds = 180) {
  if (-not (Test-Path "$Dir/graphics-tests.dex")) {
    Write-Host "FAIL  dalvikvm: $Name (graphics-tests.dex is missing)"
    $script:failed++
    return
  }
  $bcpGraphics = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
  $psi = [Diagnostics.ProcessStartInfo]::new("$Dir/dalvikvm.exe")
  foreach ($a in @("-Xbootclasspath:$bcpGraphics", "-Ximage:$(NoDrive "$root/system")/framework/boot.art",
                   '-Xint', '-Xusejit:false', '-cp', (NoDrive "$Dir/graphics-tests.dex"), "gesher.$Main") + $Arguments) {
    $psi.ArgumentList.Add($a)
  }
  $psi.Environment['GESHER_TRACE_FAULTS'] = '1'  # report a fault where it happens (crash.cc)
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  # Own the entire graph before any fallible process/reader acquisition.
  $p = [Diagnostics.Process]::new()
  $cancellation = [Threading.CancellationTokenSource]::new()
  $pipes = @(@{ Name = 'stdout'; Reader = $null; Handle = $null; Task = $null; Copied = $false },
             @{ Name = 'stderr'; Reader = $null; Handle = $null; Task = $null; Copied = $false })
  $started = $false; $kernelComplete = $false; $tasksComplete = $false; $killAttempted = $false
  $timedOut = $false; $code = -1; $out = ''; $closed = 0; $disposed = $false
  $processHandle = $null; $processId = 0; $startTicks = 0; $image = ''
  try {
    $p.StartInfo = $psi
    $started = $p.Start()
    if (-not $started) { throw 'graphics process did not start' }
    $processHandle = $p.SafeHandle
    $processId = $p.Id; $startTicks = $p.StartTime.ToUniversalTime().Ticks
    $image = [IO.Path]::GetFullPath($psi.FileName)
    $pipes[0].Reader = $p.StandardOutput
    $pipes[0].Handle = $pipes[0].Reader.BaseStream.SafeFileHandle
    $pipes[1].Reader = $p.StandardError
    $pipes[1].Handle = $pipes[1].Reader.BaseStream.SafeFileHandle
    foreach ($pipe in $pipes) { $pipe.Task = $pipe.Reader.ReadToEndAsync($cancellation.Token) }
    $kernelComplete = $p.WaitForExit($Seconds * 1000)
    $timedOut = -not $kernelComplete
    if ($timedOut) {
      $killAttempted = $true
      try { $p.Kill($true) } catch { $out += "`n(graphics kill failed: $_)" }
      $kernelComplete = $p.WaitForExit(5000)
    }
    foreach ($pipe in $pipes) {
      if ($pipe.Task.Wait(10000)) {
        $out += $pipe.Task.Result; $pipe.Copied = $true
      } else {
        $timedOut = $true
        $out += "`n($($pipe.Name) did not complete within ten seconds)"
      }
    }
    if (-not $timedOut -and $kernelComplete) { $code = $p.ExitCode }
  } catch {
    $timedOut = $true; $out += "`n(graphics process/reader failure: $_)"
  } finally {
    # An expired operation/EOF verdict is permanent. Cleanup cannot grant PASS.
    if ($started -and -not $kernelComplete) {
      $timedOut = $true
      if (-not $killAttempted) {
        $killAttempted = $true
        try { $p.Kill($true) } catch { $out += "`n(final graphics kill failed: $_)" }
        try { $kernelComplete = $p.WaitForExit(5000) } catch { $out += "`n(kernel completion unavailable: $_)" }
      }
    }
    if (-not $started) { $kernelComplete = $true }
    try { $cancellation.Cancel() } catch { $timedOut = $true; $out += "`n(cancel failed: $_)" }
    $tasks = [Threading.Tasks.Task[]]@($pipes | Where-Object { $null -ne $_.Task } | ForEach-Object { $_.Task })
    if ($tasks.Count) { try { [void][Threading.Tasks.Task]::WaitAll($tasks, 5000) } catch {} }
    $tasksComplete = @($tasks | Where-Object { -not $_.IsCompleted }).Count -eq 0
    if (-not $kernelComplete -or -not $tasksComplete) {
      $timedOut = $true
      Write-Host "FAIL  dalvikvm: $Name (failed containment; original parent passively retains process/readers/tasks)"
      # A local/static root alone dies with pwsh. Keep THIS original invocation
      # alive until settlement. No new kill, operation budget or success retry.
      # External forced termination of this owner is explicitly unqualified.
      while (-not $kernelComplete -or -not $tasksComplete) {
        if (-not $kernelComplete) { try { $kernelComplete = $p.WaitForExit(5000) } catch {} }
        foreach ($task in $tasks) { if (-not $task.IsCompleted) { try { [void]$task.Wait(5000) } catch {} } }
        $tasksComplete = @($tasks | Where-Object { -not $_.IsCompleted }).Count -eq 0
      }
    }
    # Only exact kernel AND every acquired task terminal permits disposal.
    foreach ($pipe in $pipes) {
      if ($null -ne $pipe.Task -and -not $pipe.Copied -and
          $pipe.Task.Status -eq [Threading.Tasks.TaskStatus]::RanToCompletion) { $out += $pipe.Task.Result }
      if ($null -ne $pipe.Reader) {
        try {
          if ($null -ne $pipe.Handle) {
            if ([GtestProcessNativeHandles]::CloseOwnedReader($pipe.Reader, $pipe.Handle)) { $closed++ }
            else { $timedOut = $true; $out += "`n(owned graphics pipe close not proved)" }
          } else { $pipe.Reader.Dispose(); $timedOut = $true }
        } catch { $timedOut = $true; $out += "`n(reader close failed: $_)" }
      }
    }
    try { $cancellation.Dispose() } catch { $timedOut = $true }
    try { $p.Dispose(); $disposed = $null -eq $processHandle -or $processHandle.IsClosed } catch { $timedOut = $true }
  }
  if ($env:GESHER_CI_PROCESS_TRACE -eq '1') {
    $outputHandle = if ($null -ne $pipes[0].Handle) { $pipes[0].Handle.DangerousGetHandle() } else { [IntPtr]::Zero }
    $errorHandle = if ($null -ne $pipes[1].Handle) { $pipes[1].Handle.DangerousGetHandle() } else { [IntPtr]::Zero }
    Write-Host "ACTUAL_GRAPHICS_CLEANUP pid=$processId start=$startTicks expectedimage=$image kernel=$kernelComplete tasks=$tasksComplete pipes=$closed disposed=$disposed output=$outputHandle error=$errorHandle"
  }
  if ($timedOut -or -not $disposed) { $code = -1 }
  if ($timedOut) { $out += "`n(timed out after $Seconds s)" }
  if ($code -eq 0 -and $out.Contains("$Main all passed")) {
    Write-Host "PASS  dalvikvm: $Name"
    ($out -split "`n" | Select-String -Pattern '^(PASS|FAIL)  ') | Out-Host
  } else {
    Write-Host ("FAIL  dalvikvm: $Name (exit 0x{0:X8})" -f $code)
    if (Test-Path "$Dir/dalvikvm.map") {
      $out | python "$PSScriptRoot/../../../tools/symbolize.py" "$Dir/dalvikvm.map" | Out-Host
    } else {
      Write-Host $out
    }
    $script:failed++
  }
}

# android.graphics on framework.jar with hwui's natives (Bitmap, Canvas, Paint
# text with a font file, BitmapFactory/compress), and a Surface over
# gesher-app's Win32 window (lockCanvas, PrintWindow read-back).
# The font is copied next to the test root: paths without a drive letter are
# on the current drive, which need not be C:.
Copy-Item 'C:/Windows/Fonts/arial.ttf' (Join-Path $root 'arial.ttf')
GraphicsDex 'android.graphics (Bitmap, Canvas, Paint text, codecs) and a Surface in a Win32 window' `
  GraphicsTest @((NoDrive (Join-Path $root 'arial.ttf')), (NoDrive $env:RUNNER_TEMP))

# hwui's renderer: HardwareRenderer, its RenderThread and the CPU pipeline
# drawing a RenderNode tree into the window's Surface.
GraphicsDex 'HardwareRenderer: a RenderNode tree into the Win32 window (CPU pipeline)' HardwareRendererTest


# An app's UI in a Win32 window: 34-windows.ps1 (after the fonts are installed).
