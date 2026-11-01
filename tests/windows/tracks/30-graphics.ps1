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
  # Original references survive every attempted call; FAILED never becomes DONE
  # by retry. This same invocation is the owner, including error/report sinks.
  $o = @{ Phase = 'Acquire'; Failed = $false; Launch = 'UNATTEMPTED'; P = $null;
    Cancellation = $null; CancelState = 'UNATTEMPTED'; ProcessClose = 'UNATTEMPTED';
    CancelClose = 'UNATTEMPTED'; Kernel = $false; Kill = $false; Tasks = $false;
    Handle = $null; Pid = 0; Birth = [UInt64]0; NativeExit = $null; Out = '';
    OperationExpired = $false; EofComplete = $false; MechanismFailure = $false; CleanupBudgetExpired = $false;
    Pipes = @(@{ Reader = $null; Handle = $null; Value = [Int64]0; Task = $null; Read = 'UNATTEMPTED'; Close = 'UNATTEMPTED'; Copied = $false },
              @{ Reader = $null; Handle = $null; Value = [Int64]0; Task = $null; Read = 'UNATTEMPTED'; Close = 'UNATTEMPTED'; Copied = $false }) }
  while ($true) {
    try {
      if ($o.Phase -eq 'Acquire') {
        $o.P = [Diagnostics.Process]::new()
        $o.Cancellation = [Threading.CancellationTokenSource]::new()
        $o.P.StartInfo = $psi
        $o.Launch = 'UNKNOWN' # before invocation, including interruption/unreturned
        if (-not $o.P.Start()) { throw 'process launch did not return owned child' }
        $o.Launch = 'OWNED'
        $o.Handle = $o.P.SafeHandle
        $o.Pid = $o.P.Id
        $o.Birth = [UInt64]$o.P.StartTime.ToUniversalTime().ToFileTimeUtc()
        for ($i = 0; $i -lt 2; $i++) {
          $q = $o.Pipes[$i]
          $q.Read = 'UNKNOWN'
          $q.Reader = if ($i -eq 0) { $o.P.StandardOutput } else { $o.P.StandardError }
          $q.Handle = $q.Reader.BaseStream.SafeFileHandle
          $q.Value = $q.Handle.DangerousGetHandle().ToInt64()
          $q.Task = $q.Reader.ReadToEndAsync($o.Cancellation.Token)
          $q.Read = 'OWNED'
        }
        $o.Phase = 'Observe'
      }
      if ($o.Phase -eq 'Observe') {
        $o.Kernel = $o.P.WaitForExit($Seconds * 1000)
        $o.OperationExpired = -not $o.Kernel
        if (-not $o.Kernel) { $o.Failed = $true }
        if ($o.Kernel) { $o.NativeExit = [int]$o.P.ExitCode }
        if (-not $o.Kernel -and -not $o.Kill) {
          $o.Kill = $true # one reservation BEFORE attempt
          $o.P.Kill($true)
          $o.Kernel = $o.P.WaitForExit(5000)
          if (-not $o.Kernel) { $o.CleanupBudgetExpired = $true }
          if ($o.Kernel) { $o.NativeExit = [int]$o.P.ExitCode }
        }
        $o.EofComplete = $true
        foreach ($q in $o.Pipes) {
          if ($q.Task.Wait(10000)) { $o.Out += $q.Task.Result; $q.Copied = $true }
          else { $o.Failed = $true; $o.EofComplete = $false; $o.Out += "`n(reader did not complete within ten seconds)" }
        }
        $o.Phase = 'Settle'
      }
      if ($o.Phase -eq 'Settle') {
        # Unknown launch/read outcome cannot be reconstructed from null fields.
        if ($o.Launch -eq 'UNKNOWN' -or @($o.Pipes | Where-Object { $_.Read -eq 'UNKNOWN' }).Count) {
          $o.Failed = $true; [Threading.Thread]::Sleep(5000); continue
        }
        if ($o.Launch -eq 'UNATTEMPTED') { $o.Kernel = $true }
        if (-not $o.Kernel) {
          $o.Failed = $true
          if (-not $o.Kill) { $o.Kill = $true; $o.P.Kill($true) }
          $o.Kernel = $o.P.WaitForExit(5000)
          if (-not $o.Kernel) { $o.CleanupBudgetExpired = $true }
          if ($o.Kernel) { $o.NativeExit = [int]$o.P.ExitCode }
        }
        if ($o.CancelState -eq 'UNATTEMPTED') {
          $o.CancelState = 'FAILED'
          if ($null -ne $o.Cancellation) { $o.Cancellation.Cancel() }
          $o.CancelState = 'DONE'
        }
        if ($o.CancelState -ne 'DONE') { $o.Failed = $true; [Threading.Thread]::Sleep(5000); continue }
        $o.Tasks = $true
        foreach ($q in $o.Pipes) {
          if ($q.Read -eq 'OWNED' -and -not $q.Task.IsCompleted) {
            $o.Tasks = $false
            try { if (-not $q.Task.Wait(5000)) { $o.CleanupBudgetExpired = $true } }
            catch { $o.Failed = $true; if ($q.Task.Status -ne [Threading.Tasks.TaskStatus]::Canceled) { $o.MechanismFailure = $true } }
          }
        }
        $o.Tasks = $true
        foreach ($q in $o.Pipes) { if ($q.Read -eq 'OWNED' -and -not $q.Task.IsCompleted) { $o.Tasks = $false; $o.CleanupBudgetExpired = $true } }
        if (-not $o.Kernel -or -not $o.Tasks) { $o.Failed = $true; continue }
        $o.Phase = 'Close'
      }
      if ($o.Phase -eq 'Close') {
        # No acquisition/task/kernel query is repeated after uncertain close.
        foreach ($q in $o.Pipes) {
          if ($q.Close -eq 'FAILED') { throw 'uncertain reader close retained' }
          if ($q.Close -eq 'DONE') { continue }
          if ($q.Read -eq 'OWNED' -and -not $q.Copied -and $q.Task.Status -eq [Threading.Tasks.TaskStatus]::RanToCompletion) {
            $o.Out += $q.Task.Result; $q.Copied = $true
          }
          $q.Close = 'FAILED'
          if ($null -ne $q.Reader) {
            if ($null -eq $q.Handle -or -not [GtestProcessNativeHandles]::CloseOwnedReader($q.Reader, $q.Handle)) {
              throw 'exact reader close/native invalidity unavailable'
            }
          }
          $q.Close = 'DONE'
        }
        if ($o.CancelClose -eq 'FAILED') { throw 'uncertain cancellation close retained' }
        if ($o.CancelClose -eq 'UNATTEMPTED') {
          $o.CancelClose = 'FAILED'
          if ($null -ne $o.Cancellation) { $o.Cancellation.Dispose() }
          $o.CancelClose = 'DONE'
        }
        if ($o.ProcessClose -eq 'FAILED') { throw 'uncertain process close retained' }
        if ($o.ProcessClose -eq 'UNATTEMPTED') {
          $o.ProcessClose = 'FAILED'
          if ($null -ne $o.P) { $o.P.Dispose() }
          if ($o.Launch -eq 'OWNED' -and ($null -eq $o.Handle -or -not $o.Handle.IsClosed)) { throw 'process close unproved' }
          $o.ProcessClose = 'DONE'
        }
        $o.Phase = 'Report'
      }
      if ($o.Phase -eq 'Report') {
        if ($env:GESHER_CI_PROCESS_TRACE -eq '1') {
          $script:GraphicsDexObservation = [pscustomobject]@{ Source = $env:GITHUB_SHA; Case = $env:GESHER_GRAPHICS84_MODE;
            Nonce = $env:GESHER_GRAPHICS84_NONCE; Pid = [UInt32]$o.Pid; CreationFiletime = [UInt64]$o.Birth;
            KernelComplete = [bool]$o.Kernel; NativeExitCode = $o.NativeExit; KillReserved = [bool]$o.Kill; Released = $true;
            MarkerPresent = [bool]$o.Out.Contains("$Main all passed"); OperationExpired = [bool]$o.OperationExpired;
            EofComplete = [bool]$o.EofComplete; TasksComplete = [bool]$o.Tasks;
            PipesClosed = [int]@($o.Pipes | Where-Object { $_.Close -eq 'DONE' -and $null -ne $_.Handle }).Count;
            ObservationFailed = [bool]$o.Failed; ExpectedImage = [IO.Path]::GetFullPath($psi.FileName);
            MechanismFailure = [bool]$o.MechanismFailure; CleanupBudgetExpired = [bool]$o.CleanupBudgetExpired }
          Write-Host "ACTUAL_GRAPHICS_CLEANUP pid=$($o.Pid) kernel=$($o.Kernel) tasks=$($o.Tasks) pipes=2 disposed=True output=$($o.Pipes[0].Value) error=$($o.Pipes[1].Value)"
        }
        $timedOut = [bool]$o.Failed
        $out = $o.Out
        $code = if ($o.Failed -or $null -eq $o.NativeExit) { -1 } else { $o.NativeExit }
        # All downstream report/symbolizer/root-release boundaries stay inside
        # this barrier. A throwing sink cannot discard the retained owner.
        if ($timedOut) { $out += "`n(timed out after $Seconds s)" }
        if ($code -eq 0 -and $out.Contains("$Main all passed")) {
          Write-Host "PASS  dalvikvm: $Name"
          ($out -split "`n" | Select-String -Pattern '^(PASS|FAIL)  ') | Out-Host
        } else {
          Write-Host ("FAIL  dalvikvm: $Name (exit 0x{0:X8})" -f $code)
          if (Test-Path "$Dir/dalvikvm.map") { $out | python "$PSScriptRoot/../../../tools/symbolize.py" "$Dir/dalvikvm.map" | Out-Host }
          else { Write-Host $out }
          $script:failed++
        }
        break
      }
    } catch {
      $o.Failed = $true
      $o.MechanismFailure = $true
      if ($o.Phase -eq 'Observe') { $o.EofComplete = $false }
      if ($o.Phase -in @('Acquire','Observe')) { $o.Phase = 'Settle' }
      # Protected parking; no sink/query/close is required to retain ownership.
      try { [Threading.Thread]::Sleep(5000) } catch {}
    }
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
