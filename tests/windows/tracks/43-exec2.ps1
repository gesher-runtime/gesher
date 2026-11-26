# Native processes from native code (docs/exec.md, ADR 0055): exec*(),
# system(), popen(), posix_spawn(), signals between processes, and waitpid's
# status decoding, plus the /system/bin tools gesher-exec provides. Dot-sourced
# by run-tests.ps1 (Check, NoDrive, $Dir, $failed are in scope). The test
# programs come from .github/workflows/exec.yml (the "exec" artifact):
# libexec2test.so (the driver) and libexec2child.so (the program it starts).

if (-not (Test-Path "$Dir/libexec2test.so")) {
  Write-Host 'FAIL  exec2: the exec2 artifact (libexec2test.so) is missing'
  $failed++
  return
}

$child = (Resolve-Path "$Dir/libexec2child.so").Path
$driver = (Resolve-Path "$Dir/libexec2test.so").Path
$env:LD_LIBRARY_PATH = NoDrive $Dir

# The driver runs the whole suite and prints one PASS line per feature and a
# SUMMARY; it exits 0 only if every check passed.
Check 'exec2: native processes from native code (system, popen, posix_spawn, signals, waitpid)' {
  & "$Dir/gesher-exec.exe" --elf $driver 'libexec2test.so' (NoDrive $child)
} @('exec2: PASS system runs the shell and returns its status',
    'exec2: PASS popen reads a command''s output',
    'exec2: PASS popen writes actual bytes to the shell read builtin',
    'exec2: PASS shell pipeline preserves bytes for the read builtin',
    'exec2: PASS posix_spawn with a dup2 file action, output captured',
    'exec2: PASS waitpid decodes an exit code',
    'exec2: PASS kill() to another process runs its SIGUSR1 and SIGTERM handlers',
    'exec2: PASS signal 0 finds a running process',
    'exec2: PASS SIGKILL ends a process (WIFSIGNALED)',
    'exec2: SUMMARY pass=10 fail=0')

# exec*() outside a fork replaces the image: the launcher process exits with the
# child's status (17).
$null = & "$Dir/gesher-exec.exe" --elf $driver 'libexec2test.so' '--exec-replace' (NoDrive $child) 2>&1
if ($LASTEXITCODE -eq 17) {
  Write-Host 'PASS  exec2: execv() outside a fork replaces the image (exit 17)'
} else {
  Write-Host "FAIL  exec2: execv() replace: exit $LASTEXITCODE, not 17"
  $failed++
}

Check 'exec2: execl resolves through libc and preserves register and stack arguments' {
  & "$Dir/gesher-exec.exe" --elf $driver 'libexec2test.so' '--execl-replace' (NoDrive $child)
} @('one two three four five six seven eight')

# The runtime cannot clone a Windows address space with Linux fork/vfork
# semantics. Unsupported calls must fail honestly and leave real I/O alone.
Check 'exec2: fork and vfork report ENOSYS without synthetic children or input' {
  & "$Dir/gesher-exec.exe" --elf $driver 'libexec2test.so' '--fork-unsupported'
} @('exec2: PASS fork reports ENOSYS without creating a child',
    'exec2: PASS vfork reports ENOSYS without creating a child',
    'exec2: PASS unsupported fork leaves no waitable child',
    'exec2: PASS unsupported fork leaves the caller and real pipe bytes unchanged',
    'exec2: SUMMARY pass=4 fail=0')

# Real child/grandchild descriptor sharing and separate-open atomic append.
# The expected count includes every public contract assertion in the corpus.
Check 'exec2: OFDs survive real spawn and independent processes append atomically' {
  & "$Dir/gesher-exec.exe" --elf $driver 'libexec2test.so' '--ofd-process' (NoDrive $child)
} @('exec2: PASS spawn preserves flags, aliases, shared position and CLOEXEC',
    'exec2: PASS parent F_SETFL reaches the child and a real grandchild',
    'exec2: PASS atomic append loses or overwrites no complete record across processes',
    'exec2: SUMMARY pass=11 fail=0')

# The /system/bin programs gesher-exec provides (docs/exec.md): the minimal
# shell runs them as external commands.
$lsOut = & "$Dir/gesher-exec.exe" --sh -c "ls $(NoDrive $Dir)" 2>&1 | Out-String
if ($lsOut -match 'libexec2test\.so') {
  Write-Host 'PASS  exec2: /system/bin/ls lists a directory'
} else {
  Write-Host "FAIL  exec2: ls: $($lsOut.Trim())"
  $failed++
}

$idOut = & "$Dir/gesher-exec.exe" --sh -c 'id' 2>&1 | Out-String
if ($idOut -match 'uid=\d+') {
  Write-Host 'PASS  exec2: /system/bin/id prints the uid'
} else {
  Write-Host "FAIL  exec2: id: $($idOut.Trim())"
  $failed++
}

# getprop reads $ANDROID_ROOT/build.prop; with none set it falls back to the
# default argument, as a device's getprop does.
$propOut = & "$Dir/gesher-exec.exe" --sh -c 'getprop ro.none.here fallback-value' 2>&1 | Out-String
if ($propOut -match 'fallback-value') {
  Write-Host 'PASS  exec2: /system/bin/getprop returns a default for an unset property'
} else {
  Write-Host "FAIL  exec2: getprop: $($propOut.Trim())"
  $failed++
}

# Device identity (docs/device-identity.md): a new property area (its own
# session) takes ro.product.manufacturer/model from the PC's SMBIOS, which
# Windows' Win32_ComputerSystem also reports, and the rest of the identity from
# build.prop, the same with ANDROID_ROOT drive-relative and as /mnt/<drive>/.
$identityBinder = $env:GESHER_BINDER
$identitySession = $env:GESHER_PROPERTY_SESSION
$identityRoot = $env:ANDROID_ROOT
# Placeholder() in compat/posix/include/gesher/device_identity.h, the same
# lists (tools/test_device_identity.py checks that they match): a value it treats
# as a firmware placeholder is not expected verbatim.
$identityPlaceholders = @(
  'to be filled by o.e.m.', 'to be filled by o.e.m', 'to be filled by oem', 'system manufacturer',
  'system manufacture', 'system product name', 'system product', 'system name', 'system version',
  'system family', 'system sku', 'system serial number', 'default string', 'default',
  'not applicable', 'not specified', 'not available', 'not defined', 'none', 'n/a', 'na',
  'unknown', 'undefined', 'invalid', 'empty', 'null', 'oem', 'o.e.m.', 'oem name', 'oem product',
  'manufacturer', 'product name', 'product', 'board manufacturer', 'board product name',
  'base board manufacturer', 'base board product name', 'baseboard manufacturer',
  'baseboard product name', 'motherboard', 'mainboard', 'family', 'sku', '123456789', '1234567890',
  '0123456789', 'chassis manufacturer', 'x.x', 'insyde', 'insyde corp.')
$identityPlaceholderPrefixes = @('type1', 'type2 - ', 'type2-')
function IdentityPlaceholder([string]$value) {
  if (-not $value) { return $true }
  if ($value -notmatch '[0-9A-Za-z]|[^\x00-\x7f]') { return $true }
  if ($value -cmatch '^([\x00-\x7f])\1*$') { return $true }
  if ($identityPlaceholders -contains $value) { return $true }
  foreach ($prefix in $identityPlaceholderPrefixes) {
    if ($value.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { return $true }
  }
  return $false
}
$computer = Get-CimInstance Win32_ComputerSystem
$computerProduct = Get-CimInstance Win32_ComputerSystemProduct
$expectedManufacturer = "$($computer.Manufacturer)".Trim() -replace '\s+', ' '
$expectedModel = "$($computer.Model)".Trim() -replace '\s+', ' '
$lenovoVersion = "$($computerProduct.Version)".Trim() -replace '\s+', ' '
if ($expectedManufacturer -eq 'LENOVO' -and -not (IdentityPlaceholder $lenovoVersion) -and $lenovoVersion -ne 'LENOVO') {
  $expectedModel = $lenovoVersion
}
$systemRoot = (Resolve-Path (Join-Path $root 'system')).Path
$identityForms = [ordered]@{
  'drive-relative' = NoDrive $systemRoot
  'mnt' = '/mnt/' + $systemRoot.Substring(0, 1).ToLowerInvariant() + ($systemRoot.Substring(2) -replace '\\', '/')
}
$identities = @{}
try {
  Remove-Item Env:GESHER_BINDER -ErrorAction SilentlyContinue
  foreach ($form in $identityForms.Keys) {
    $env:ANDROID_ROOT = $identityForms[$form]
    $env:GESHER_PROPERTY_SESSION = "device-identity-$form-$PID-$([DateTime]::UtcNow.Ticks)"
    $listing = & "$Dir/gesher-exec.exe" --sh -c 'getprop' 2>&1 | Out-String
    $props = @{}
    foreach ($m in [regex]::Matches($listing, '(?m)^\[([^\]]+)\]: \[(.*)\]\r?$')) { $props[$m.Groups[1].Value] = $m.Groups[2].Value }
    $identities[$form] = $props
    $problems = @()
    $fingerprint = '{0}/{1}/{2}:{3}/{4}/{5}:{6}/{7}' -f $props['ro.product.brand'], $props['ro.product.name'],
      $props['ro.product.device'], $props['ro.build.version.release'], $props['ro.build.id'],
      $props['ro.build.version.incremental'], $props['ro.build.type'], $props['ro.build.tags']
    if (-not $props['ro.build.fingerprint']) { $problems += "no build.prop values (ANDROID_ROOT $env:ANDROID_ROOT)" }
    elseif ($props['ro.build.fingerprint'] -cne $fingerprint) { $problems += "fingerprint $($props['ro.build.fingerprint']) is not $fingerprint" }
    foreach ($name in 'ro.product.brand', 'ro.product.name', 'ro.product.device', 'ro.product.board', 'ro.hardware', 'ro.bootloader') {
      if ($props[$name] -cne 'gesher') { $problems += "$name=$($props[$name])" }
    }
    if ($props['ro.build.flavor'] -ne 'gesher-user') { $problems += "ro.build.flavor=$($props['ro.build.flavor'])" }
    if (-not $props['ro.product.manufacturer'] -or -not $props['ro.product.model']) { $problems += 'manufacturer or model unset' }
    if (-not (IdentityPlaceholder $expectedManufacturer) -and $props['ro.product.manufacturer'] -cne $expectedManufacturer) {
      $problems += "manufacturer $($props['ro.product.manufacturer']) is not SMBIOS's $expectedManufacturer"
    }
    if (-not (IdentityPlaceholder $expectedModel) -and $props['ro.product.model'] -cne $expectedModel) {
      $problems += "model $($props['ro.product.model']) is not SMBIOS's $expectedModel"
    }
    if ($listing -match '(?i)shiba|pixel 8|\[google\]') { $problems += 'a Pixel 8 identity remains' }
    if ($LASTEXITCODE -eq 0 -and -not $problems) {
      Write-Host "PASS  exec2: device identity is the PC's ($($props['ro.product.manufacturer']) / $($props['ro.product.model'])) and Gesher's fingerprint, ANDROID_ROOT $form"
    } else {
      Write-Host "FAIL  exec2: device identity, ANDROID_ROOT $form (exit $LASTEXITCODE): $($problems -join '; ')"
      Write-Host $listing
      $failed++
    }
  }
  $same = $true
  foreach ($name in 'ro.product.manufacturer', 'ro.product.model', 'ro.build.fingerprint') {
    if ($identities['drive-relative'][$name] -cne $identities['mnt'][$name]) { $same = $false }
  }
  if ($same) { Write-Host 'PASS  exec2: one device identity whatever form ANDROID_ROOT takes' }
  else { Write-Host 'FAIL  exec2: the device identity differs between drive-relative and /mnt ANDROID_ROOT'; $failed++ }
} finally {
  $env:ANDROID_ROOT = $identityRoot
  if ($null -eq $identitySession) { Remove-Item Env:GESHER_PROPERTY_SESSION -ErrorAction SilentlyContinue }
  else { $env:GESHER_PROPERTY_SESSION = $identitySession }
  if ($null -ne $identityBinder) { $env:GESHER_BINDER = $identityBinder }
}

Remove-Item Env:LD_LIBRARY_PATH
