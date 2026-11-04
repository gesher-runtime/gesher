# Regression checks of app-visible fixes in gesher-app's services and boot class
# path, run from services-tests.dex (tests/dex-services) with the interpreter.
# Dot-sourced by run-tests.ps1, using Check, NoDrive, $Dir, $root and $failed
# from there.

# Runs dalvikvm with a time limit, killing it (and what it started) on time-out.
function FixesRun([string[]] $Arguments, [int] $Seconds) {
  $psi = [Diagnostics.ProcessStartInfo]::new("$Dir/dalvikvm.exe")
  foreach ($a in $Arguments) { $psi.ArgumentList.Add($a) }
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $p = [Diagnostics.Process]::Start($psi)
  $stdout = $p.StandardOutput.ReadToEndAsync()
  $stderr = $p.StandardError.ReadToEndAsync()
  $timedOut = -not $p.WaitForExit($Seconds * 1000)
  if ($timedOut) {
    $p.Kill($true)
    $p.WaitForExit()
  }
  $null = $stdout.Wait(10000), $stderr.Wait(10000)
  $out = $stdout.Result + $stderr.Result
  if ($timedOut) { $out += "`n(timed out after $Seconds s)" }
  $global:LASTEXITCODE = if ($timedOut) { -1 } else { $p.ExitCode }
  return $out
}

Copy-Item "$Dir/framework-res.apk" (Join-Path $root 'system/framework') -Force
Copy-Item "$Dir/build.prop" (Join-Path $root 'system') -Force
$bcpFixes = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$dvmFixes = @("-Xbootclasspath:$bcpFixes", "-Ximage:$(NoDrive "$root/system")/framework/boot.art", '-Xint', '-Xusejit:false',
  '-cp', (NoDrive "$Dir/services-tests.dex"))

# The document picker (ACTION_OPEN_DOCUMENT / _TREE, ActivityTaskManagerService)
# shows the Windows dialog on a thread of its own: startActivity returns while it
# is up, not when the user closes it (the app's UI froze meanwhile); the picked
# Windows path is mapped to Gesher's (Zygote.fromWindowsPath: the own drive's
# without a drive, another's under /mnt/<drive>). The test exits with the dialog
# still open.
$pickerOut = FixesRun ($dvmFixes + 'gesher.services.DocumentPickerTest') 90
$pickerCode = $LASTEXITCODE
Check 'services: the document picker does not block startActivity; picked Windows paths map to Gesher''s' {
  $global:LASTEXITCODE = $pickerCode
  $pickerOut
} @('DocumentPickerTest: fromWindowsPath maps the own drive to /, another to /mnt/<drive>',
  'DocumentPickerTest: startActivity returns at once while the picker is up; Windows paths mapped passed')
foreach ($m in [regex]::Matches($pickerOut, 'DocumentPickerTest: (android\.intent\.action\.\S+) (dialog [^\r\n]*)')) {
  Write-Host "INFO  services: document picker: $($m.Groups[1].Value): $($m.Groups[2].Value)"
}

# android.graphics.ColorMatrix on the boot class path (framework.jar lacks it), and
# FileChannel locks over FileDispatcherImpl.lock0/release0 (fcntl record locks).
$fixesOut = FixesRun ($dvmFixes + 'gesher.services.FrameworkFixesTest') 90
$fixesCode = $LASTEXITCODE
Check 'services: android.graphics.ColorMatrix is on the boot class path and computes' {
  $global:LASTEXITCODE = $fixesCode
  $fixesOut
} @('FrameworkFixesTest: ColorMatrix on the boot class path: identity, setSaturation, setScale, postConcat',
  'FrameworkFixesTest all passed')
Check 'services: FileChannel lock, tryLock and release (FileDispatcherImpl over fcntl record locks)' {
  $global:LASTEXITCODE = $fixesCode
  $fixesOut
} @('FrameworkFixesTest: FileChannel lock, tryLock (shared, ranges) and release',
  'FrameworkFixesTest all passed')
Check 'services: android.view.RenderNode and DisplayListCanvas (pre-API-29 names) for older UI libraries' {
  $global:LASTEXITCODE = $fixesCode
  $fixesOut
} @('FrameworkFixesTest: android.view.RenderNode and DisplayListCanvas with create, start, end, drawRenderNode',
  'FrameworkFixesTest all passed')
Check 'services: android.net.nsd.NsdManager (the Connectivity module''s API) on the boot class path' {
  $global:LASTEXITCODE = $fixesCode
  $fixesOut
} @('FrameworkFixesTest: android.net.nsd.NsdManager: discovery starts, registration fails through its listener, unregistering does not throw',
  'FrameworkFixesTest all passed')
Check 'services: a split APK install takes its native libraries and ABI from the ABI split (config.arm64_v8a.apk)' {
  $global:LASTEXITCODE = $fixesCode
  $fixesOut
} @('FrameworkFixesTest: NativeLibs: the package ABI and libraries come from its ABI split (config.arm64_v8a.apk), not base.apk alone',
  'FrameworkFixesTest all passed')
Check 'services: a stand-in service answers an empty list, not null, for a list method it does not implement' {
  $global:LASTEXITCODE = $fixesCode
  $fixesOut
} @('FrameworkFixesTest: a stand-in''s unimplemented list method answers an empty list (IShortcutService.getShortcuts), not null',
  'FrameworkFixesTest all passed')
Check 'services: android.text.Html.fromHtml parses with tagsoup (the ext boot jar''s classes in gesher-app.jar)' {
  $global:LASTEXITCODE = $fixesCode
  $fixesOut
} @('FrameworkFixesTest: android.text.Html.fromHtml parses with tagsoup (the ext boot jar''s): text, entities, a bold span and a link',
  'FrameworkFixesTest all passed')
Check 'services: android.net.DnsResolver (the Connectivity module''s API) resolves through Windows' {
  $global:LASTEXITCODE = $fixesCode
  $fixesOut
} @('FrameworkFixesTest: android.net.DnsResolver: query and rawQuery answer localhost through the executor, other types NOTIMP, a cancelled query is silent',
  'FrameworkFixesTest all passed')
Check 'services: BluetoothManager without an adapter, and android.os.Debug''s native heap counters' {
  $global:LASTEXITCODE = $fixesCode
  $fixesOut
} @('FrameworkFixesTest: BluetoothManager without an adapter; Debug.getNativeHeapSize, getNativeHeapAllocatedSize and getNativeHeapFreeSize',
  'FrameworkFixesTest all passed')
