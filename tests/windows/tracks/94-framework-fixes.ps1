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

# Exercise the MediaStore implementation from the production boot jar.
$mediaStoreOut = FixesRun ($dvmFixes + 'gesher.services.MediaStoreContractTest') 90
$mediaStoreCode = $LASTEXITCODE
Check 'services: MediaStore picker/collection contracts and missing-provider handling' {
  $global:LASTEXITCODE = $mediaStoreCode
  $mediaStoreOut
} @('MediaStoreContractTest all passed: 28')

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
Check 'services: libphonenumber (the ext boot jar''s, in framework.jar with its metadata) parses and formats numbers' {
  $global:LASTEXITCODE = $fixesCode
  $fixesOut
} @('FrameworkFixesTest: libphonenumber on the boot class path: parse, format (E164, INTERNATIONAL, NATIONAL), findNumbers and PhoneNumberUtils.formatNumber',
  'FrameworkFixesTest all passed')

Check 'services: HandlerThread.quit with tracing off (MessageQueue''s Perfetto counter event; Gmail''s emojiCompat thread)' {
  $global:LASTEXITCODE = $fixesCode
  $fixesOut
} @('FrameworkFixesTest: HandlerThread.quit after a message, and every PerfettoTrackEventExtra class initializes with tracing off',
  'FrameworkFixesTest all passed')

# org.apache.http.legacy (external/apache-http), a shared library apps name in
# <uses-library>, not on the boot class path: laid out as gesher-app.ps1 installs it
# ($ANDROID_ROOT/framework and etc/permissions, the declaration Gesher ships), in a
# system root of its own so the other tracks' apps do not get it implicitly.
$httpRoot = Join-Path $root 'http-legacy-system'
New-Item -ItemType Directory -Force (Join-Path $httpRoot 'framework'), (Join-Path $httpRoot 'etc/permissions') | Out-Null
Copy-Item "$Dir/org.apache.http.legacy.jar" (Join-Path $httpRoot 'framework') -Force
Copy-Item (Join-Path $PSScriptRoot '../../../tools/package/etc/permissions/org.apache.http.legacy.xml') (Join-Path $httpRoot 'etc/permissions') -Force
$httpOut = FixesRun ($dvmFixes + 'gesher.services.HttpLegacyTest' + (NoDrive $httpRoot)) 90
$httpCode = $LASTEXITCODE
Check 'services: org.apache.http.legacy resolves from its declaration to the system root''s jar' {
  $global:LASTEXITCODE = $httpCode
  $httpOut
} @('HttpLegacyTest: org.apache.http.legacy resolves to $ANDROID_ROOT/framework/org.apache.http.legacy.jar',
  'HttpLegacyTest all passed')
Check 'services: apps targeting API 27 or lower get org.apache.http.legacy implicitly' {
  $global:LASTEXITCODE = $httpCode
  $httpOut
} @('HttpLegacyTest: apps targeting API 27 or lower get it implicitly, API 28 and up only by name',
  'HttpLegacyTest all passed')
Check 'services: BasicHttpParams (Gmail''s account setup) loads from org.apache.http.legacy.jar' {
  $global:LASTEXITCODE = $httpCode
  $httpOut
} @('HttpLegacyTest: BasicHttpParams, HttpConnectionParams and HttpGet load from the jar, not the boot class path',
  'HttpLegacyTest all passed')

# An installed APK's signers (ApkPackage), which apps read for their own: WhatsApp's
# registration token is built from GET_SIGNATURES. Its fixtures (tests/dex-services/
# fixtures, generate.sh) are signed as WhatsApp is: a key rotated for SDK 33 and up.
$signingOut = FixesRun ($dvmFixes + 'gesher.services.SigningTest' + (NoDrive (Join-Path $PSScriptRoot '../../dex-services/fixtures'))) 90
$signingCode = $LASTEXITCODE
Check 'services: GET_SIGNATURES answers a rotated APK''s oldest key, as WhatsApp''s token needs' {
  $global:LASTEXITCODE = $signingCode
  $signingOut
} @('SigningTest: GET_SIGNATURES answers a rotated APK''s oldest key',
  'SigningTest: GET_SIGNING_CERTIFICATES answers the current key and its rotation history',
  'SigningTest all passed')
Check 'services: a v2-signed APK answers its key, an unsigned one none' {
  $global:LASTEXITCODE = $signingCode
  $signingOut
} @('SigningTest: a v2-signed APK answers its key, an unsigned one none',
  'SigningTest all passed')
