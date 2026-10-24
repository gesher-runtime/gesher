# Runs on a Windows machine against the CI build output.
#   pwsh tests/windows/run-tests.ps1 <dir with *.exe, hello.dex, hello.apk>
param([Parameter(Mandatory)] [string] $Dir)

$ErrorActionPreference = 'Stop'
$failed = 0

function Check([string] $Name, [scriptblock] $Run, [string[]] $Expect) {
  $out = & $Run 2>&1 | Out-String
  $missing = $Expect | Where-Object { -not $out.Contains($_) }
  if ($LASTEXITCODE -eq 0 -and -not $missing) {
    Write-Host "PASS  $Name"
  } else {
    Write-Host "FAIL  $Name (exit $LASTEXITCODE; missing: $($missing -join ', '))"
    Write-Host $out
    $script:failed++
  }
}

Check 'platform probe' { & "$Dir/platform_probe.exe" } @('0 probe(s) failed')
Check 'socket probe' { & "$Dir/socket_probe.exe" } @('all passed')
Check 'epoll probe' { & "$Dir/epoll_probe.exe" } @('all passed')
Check 'libutils probe' { & "$Dir/libutils_probe.exe" } @('all passed')

Check 'dexdump: disassemble .dex' { & "$Dir/dexdump.exe" -d "$Dir/hello.dex" } @(
  "Class descriptor  : 'Lgesher/Hello;'",
  "name          : 'main'",
  'const-string v0, "Hello from ART on Windows"')

Check 'dexdump: read .dex from compressed .apk' { & "$Dir/dexdump.exe" -f "$Dir/hello.apk" } @(
  "hello.apk', DEX version '038'",
  'class_defs_size     : 1')

# The runtime starts.
Check 'dalvikvm: runtime starts' { & "$Dir/dalvikvm.exe" -showversion } @('ART version')

# dalvikvm runs Hello with the interpreter: without a boot image, the runtime
# defines and links the core classes from the boot jars, loads libcore's
# native libraries (libicu_jni maps ICU's data) and runs gesher.Hello.main.
# ART separates class path entries with ':', so paths are given without the
# drive letter.
function NoDrive([string] $p) { ($p -replace '\\', '/') -replace '^[A-Za-z]:', '' }
$root = Join-Path $env:RUNNER_TEMP 'gesher-root'
foreach ($d in 'system/framework', 'art', 'data', 'i18n/etc/icu', 'tzdata') { New-Item -ItemType Directory -Force (Join-Path $root $d) | Out-Null }
# ICU's data, where the i18n module keeps it on Android.
Copy-Item "$Dir/icudt*l.dat" (Join-Path $root 'i18n/etc/icu')
$env:ANDROID_ROOT = NoDrive "$root/system"
$env:ANDROID_ART_ROOT = NoDrive "$root/art"
$env:ANDROID_DATA = NoDrive "$root/data"
$env:ANDROID_I18N_ROOT = NoDrive "$root/i18n"
$env:ANDROID_TZDATA_ROOT = NoDrive "$root/tzdata"
# Gesher's boot image (docs/boot-image.md), where -Ximage below looks for it:
# with GESHER_REQUIRE_BOOT_IMAGE, a run that cannot load it fails instead of
# falling back to imageless running (tracks/60-boot-image.ps1 times both).
if (Test-Path "$Dir/framework/x86_64/boot.art") {
  New-Item -ItemType Directory -Force (Join-Path $root 'system/framework/x86_64') | Out-Null
  Copy-Item "$Dir/framework/x86_64/*" (Join-Path $root 'system/framework/x86_64')
  $env:GESHER_REQUIRE_BOOT_IMAGE = '1'
}
$bcp = (('core-oj', 'core-libart', 'core-icu4j') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$env:GESHER_TRACE_FAULTS = '1'  # report a fault where it happens (crash.cc)
$out = & "$Dir/dalvikvm.exe" "-Xbootclasspath:$bcp" "-Ximage:$(NoDrive "$root/system")/framework/boot.art" `
  -Xint '-Xusejit:false' '-verbose:jni' -cp (NoDrive "$Dir/hello.dex") gesher.Hello 2>&1 | Out-String
$code = $LASTEXITCODE
Remove-Item Env:GESHER_TRACE_FAULTS
if ($code -eq 0 -and $out.Contains('Hello from ART on Windows')) {
  Write-Host 'PASS  dalvikvm: runs Hello with the interpreter'
} else {
  Write-Host ('FAIL  dalvikvm: runs Hello with the interpreter (exit 0x{0:X8})' -f $code)
  # Addresses as symbols, from the linker map (tools/symbolize.py).
  if (Test-Path "$Dir/dalvikvm.map") {
    $out | python "$PSScriptRoot/../../tools/symbolize.py" "$Dir/dalvikvm.map" | Out-Host
  } else {
    Write-Host $out
  }
  $failed++
}

# Runs a class from natives.dex with the interpreter; passes when it exits 0
# and prints "all passed" (tests/dex-natives).
function RunDex([string] $Name, [string] $Main, [string[]] $Extra = @(), [string[]] $Arguments = @(), [string] $Bcp = $bcp) {
  $out = & "$Dir/dalvikvm.exe" "-Xbootclasspath:$Bcp" "-Ximage:$(NoDrive "$root/system")/framework/boot.art" `
    -Xint '-Xusejit:false' @Extra -cp (NoDrive "$Dir/natives.dex") $Main @Arguments 2>&1 | Out-String
  if ($LASTEXITCODE -eq 0 -and $out.Contains('all passed')) {
    Write-Host "PASS  dalvikvm: $Name"
  } else {
    Write-Host ("FAIL  dalvikvm: $Name (exit 0x{0:X8})" -f $LASTEXITCODE)
    if (Test-Path "$Dir/dalvikvm.map") {
      $out | python "$PSScriptRoot/../../tools/symbolize.py" "$Dir/dalvikvm.map" | Out-Host
    } else {
      Write-Host $out
    }
    $script:failed++
  }
}

# The framework natives (docs/framework-natives.md): libandroid_runtime's
# registrations and the @CriticalNative ABI canary (step 2), then the
# framework's message loop over its MessageQueue natives with both queue
# implementations (step 3), then libandroidfw through the asset natives on
# res.apk (step 4).
RunDex 'framework natives (libandroid_runtime)' gesher.NativeTest
RunDex 'Looper, Handler, HandlerThread (legacy MessageQueue)' gesher.LooperTest
RunDex 'Looper, Handler, HandlerThread (concurrent MessageQueue)' gesher.LooperTest @('-Dgesher.mq.concurrent=true')
RunDex 'resources: ApkAssets, AssetManager, XmlBlock on res.apk' gesher.ResourcesTest @() @(NoDrive "$Dir/res.apk")

# libopenjdk's EPoll natives over gesher-base's epoll (tests/dex-libcore).
$out = & "$Dir/dalvikvm.exe" "-Xbootclasspath:$bcp" "-Ximage:$(NoDrive "$root/system")/framework/boot.art" `
  -Xint '-Xusejit:false' -cp (NoDrive "$Dir/libcore-tests.dex") gesher.EPollTest 2>&1 | Out-String
if ($LASTEXITCODE -eq 0 -and $out.Contains('EPollTest all passed')) {
  Write-Host 'PASS  dalvikvm: EPoll natives over a pipe'
} else {
  Write-Host ('FAIL  dalvikvm: EPoll natives over a pipe (exit 0x{0:X8})' -f $LASTEXITCODE)
  Write-Host $out
  $failed++
}

# framework.jar on the boot class path (tests/dex-framework): its classes
# that need no natives run.
$bcpFramework = (('core-oj', 'core-libart', 'core-icu4j', 'framework') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$out = & "$Dir/dalvikvm.exe" "-Xbootclasspath:$bcpFramework" "-Ximage:$(NoDrive "$root/system")/framework/boot.art" `
  -Xint '-Xusejit:false' -cp (NoDrive "$Dir/framework-tests.dex") gesher.FrameworkSmoke 2>&1 | Out-String
if ($LASTEXITCODE -eq 0 -and $out.Contains('FrameworkSmoke all passed')) {
  Write-Host 'PASS  dalvikvm: framework.jar on the boot class path'
} else {
  Write-Host ('FAIL  dalvikvm: framework.jar on the boot class path (exit 0x{0:X8})' -f $LASTEXITCODE)
  Write-Host $out
  $failed++
}

# libandroid_runtime on framework.jar (steps 3 and 5): every table registered
# on the framework's own classes, MessageQueue, Parcel and an in-process Binder.
RunDex 'framework natives on framework.jar: MessageQueue, Parcel, Binder' gesher.FrameworkNativesTest @() @() $bcpFramework

# gesher-app drives lifecycle.apk's Activity to onResume (app/README.md): the
# app's manifest, its resources and framework-res.apk's theme are read, and
# the Application and the Activity go through their callbacks.
Copy-Item "$Dir/framework-res.apk" (Join-Path $root 'system/framework')
Copy-Item "$Dir/build.prop" (Join-Path $root 'system')
$bcpApp = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$out = & "$Dir/dalvikvm.exe" "-Xbootclasspath:$bcpApp" "-Ximage:$(NoDrive "$root/system")/framework/boot.art" `
  -Xint '-Xusejit:false' com.gesher.app.Main (NoDrive "$Dir/lifecycle.apk") 2>&1 | Out-String
$code = $LASTEXITCODE
$expect = @(
  'LifecycleApplication onCreate, package=com.gesher.lifecycle',
  'gesher-app: com.gesher.lifecycle.LifecycleActivity onResume',
  'label=Lifecycle Activity, greeting=Hello from a resource, theme attr=from-theme, windowActionBar=false',
  'gesher-app: done')
$missing = $expect | Where-Object { -not $out.Contains($_) }
if ($code -eq 0 -and -not $missing) {
  Write-Host 'PASS  gesher-app: lifecycle.apk to onResume'
} else {
  Write-Host ('FAIL  gesher-app: lifecycle.apk to onResume (exit 0x{0:X8}; missing: {1})' -f $code, ($missing -join ' | '))
  ($out -split "`n" | Select-String -Pattern 'gesher-app|Lifecycle|Exception|Error|at ' | Select-Object -First 60) | Out-Host
  $failed++
}

# Each line of work keeps its checks in a file of its own (graphics, native
# libraries, services, ...), run here in this script's scope: Check, RunDex,
# NoDrive, $root, $bcp and $failed are theirs to use.
foreach ($t in Get-ChildItem (Join-Path $PSScriptRoot 'tracks') -Filter *.ps1 -ErrorAction SilentlyContinue | Sort-Object Name) {
  . $t.FullName
}

Write-Host "`n$failed test(s) failed"
exit $failed
