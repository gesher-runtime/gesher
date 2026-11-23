# End-to-end: the Gesher installer runs real, unmodified open-source Android apps
# (docs/e2e.md). For each app of tests/e2e/apps.json (F-Droid, pinned by version and
# SHA-256): download it, `gesher-app install` it, launch it on the Gesher device with an
# off-desktop window, wait for its first frame, check the window is not blank (PrintWindow:
# non-background and text pixels), save a PNG, post input (a click, keys) and check the
# window changes, close it (WM_CLOSE) and check the app's process ends. One app is also
# started through the .apk association's registered command and the Start menu shortcut's
# command line. Prints one PASS or FAIL line per check; a FAIL carries the first exception
# of the app's log. Apps with `knownFailure` print KNOWN-FAIL and do not fail the run.
#
#   pwsh tests/e2e/run-e2e.ps1 -Setup installer\gesher-setup-*.exe -Out e2e-results
#
# Run by the e2e job of .github/workflows/ci.yml on windows-2022.
param(
  [Parameter(Mandatory)] [string] $Setup,
  [string] $Manifest = (Join-Path $PSScriptRoot 'apps.json'),
  [Alias('Out')] [string] $OutDir = 'e2e-results',
  [string[]] $Only = @(),
  [switch] $SkipInstall,
  [switch] $RequireApkIdentity,
  [string] $BuildTools = '',
  [string] $InvocationId = ([guid]::NewGuid().ToString('N'))
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $PSScriptRoot 'apks.ps1')
. (Join-Path $PSScriptRoot 'evidence.ps1')

New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
Open-E2eEvidenceRun $OutDir $InvocationId
try {
$summary = Join-Path $OutDir 'summary.txt'
Set-Content $summary ''
$script:failed = 0
$script:lines = @()
$manifestData = Get-Content -Raw $Manifest | ConvertFrom-Json
New-E2eEvidence $manifestData $Manifest $Only $OutDir $RequireApkIdentity.IsPresent
. (Join-Path $root 'tests\windows\graphics\WindowChecks.ps1')
Add-Type -AssemblyName System.Drawing

function Report([string] $line) {
  Write-Host $line
  Add-Content $summary $line
  $script:lines += $line
  if ($env:GITHUB_STEP_SUMMARY) { Add-Content $env:GITHUB_STEP_SUMMARY $line }
}

. (Join-Path $PSScriptRoot 'window-workload.ps1')

# --- Install Gesher.
$app = Join-Path $env:LOCALAPPDATA 'Programs\Gesher'
if (-not $SkipInstall) {
  $setupExe = (Resolve-Path $Setup).Path
  $script:E2eEvidence.runtime.installation_mode = 'installer'
  $script:E2eEvidence.runtime.installer_sha256 = (Get-FileHash -LiteralPath $setupExe -Algorithm SHA256).Hash.ToLowerInvariant()
  Write-E2eEvidence
  Start-Process $setupExe -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -Wait
} else {
  $script:E2eEvidence.runtime.installation_mode = 'existing_install'
  Write-E2eEvidence
}
if (-not (Test-Path "$app\gesher-app.cmd")) { throw "Gesher is not installed in $app" }
Set-E2eRuntime $app
Report "INFO  e2e: Gesher installed in $app"

$apks = Join-Path $env:TEMP 'gesher-e2e'  # on Gesher's drive
New-Item -ItemType Directory -Force $apks | Out-Null
$env:GESHER_SHORTCUTS = $null
$passed = @()

foreach ($a in $manifestData.apps) {
  $pkg = $a.package
  if ($Only.Count -and $Only -notcontains $pkg) { continue }
  $name = "$pkg $($a.versionName) ($($a.label))"
  $known = $a.knownFailure
  $problems = @()
  $info = @()
  $ranWindow = $false
  $downloadFailed = $false
  $identityFailed = $false
  Set-E2eCheck $pkg 'launch' 'running'
  # --- Download and verify.
  $apk = Join-Path $apks "$($pkg)_$($a.versionCode).apk"
  $url = "$($manifestData.repo)/$($pkg)_$($a.versionCode).apk"
  try {
    Get-PinnedApk $apk "$($pkg)_$($a.versionCode).apk" $a.sha256 (@(@($manifestData.repo) + @($manifestData.mirrors)) | Where-Object { $_ })
    $sha = (Get-FileHash -Algorithm SHA256 $apk).Hash.ToLowerInvariant()
    if (-not $a.sha256) { Report "INFO  e2e: $pkg $($a.versionCode): sha256 $sha (not pinned yet)" }
    elseif ($sha -ne $a.sha256.ToLowerInvariant()) { throw "SHA-256 $sha, not the pinned $($a.sha256)" }
    Add-E2eApk $pkg $sha
  } catch {
    $downloadFailed = $true
    $problems += "download $url failed: $_"
  }
  # --- Measure the identity of the pinned bytes with the SDK, without running
  # APK code. An unavailable optional collector stays explicit; CI requires it.
  if (-not $problems) {
    $identityArgs = @((Join-Path $PSScriptRoot 'apk_identity.py'), '--apk', $apk,
      '--package', $pkg, '--version-code', [string]$a.versionCode, '--sha256', $sha)
    if ($BuildTools) { $identityArgs += @('--build-tools', $BuildTools) }
    $identityErrorPath = Join-Path $OutDir "$pkg.identity-error.log"
    $identityJson = & python @identityArgs 2> $identityErrorPath | Out-String
    if ($LASTEXITCODE -eq 0) {
      Set-E2eApkIdentity $pkg ($identityJson | ConvertFrom-Json)
    } else {
      Set-E2eApkIdentity $pkg $null "SDK identity collection failed; see $pkg.identity-error.log"
      if ($RequireApkIdentity) {
        $identityFailed = $true
        $problems += 'Required SDK APK identity collection failed'
      }
      else { $info += 'APK identity unmeasured: SDK collection failed' }
    }
  }
  # --- Install.
  if (-not $problems) {
    $installOut = & "$app\gesher-app.cmd" install $apk 2>&1 | Out-String
    Set-Content (Join-Path $OutDir "$pkg.install.log") $installOut
    if ($LASTEXITCODE -ne 0 -or $installOut -notmatch "installed $([regex]::Escape($pkg))|$([regex]::Escape($pkg)) is installed") {
      $problems += "gesher-app install failed (exit $LASTEXITCODE): $(FirstProblem $installOut)"
    } else {
      $parsed = [regex]::Match($installOut, 'parsed by PackageParser2[^\r\n]*')
      if ($parsed.Success) {
        $info += $parsed.Value
        if ($parsed.Value -match 'targetSdk ([0-9]+)') {
          ($script:E2eEvidence.apks | Where-Object { $_.package -eq $pkg }).target_sdk = [int]$Matches[1]
          Write-E2eEvidence
        }
      }
    }
  }
  # --- Launch with an off-desktop window, check it, use it, close it.
  if (-not $problems) {
    # "trace": true: the app's process streams ART's method trace into the results
    # (GESHER_APP_METHOD_TRACE, docs/services.md), on a device started for it alone.
    # "display": "WxH": the app's device has that display (GESHER_DISPLAY), e.g. a
    # landscape one for an app whose activity asks for landscape (Gesher does not rotate
    # windows for screenOrientation yet), on a device started for it alone.
    $traceDir = $null
    $ownDevice = $a.trace -or $a.display -or $a.surfaceTrace
    $surfaceTraceBefore = $env:GESHER_SURFACE_TRACE_INVALIDATION
    $windowDebugBefore = $env:GESHER_WINDOW_DEBUG
    if ($ownDevice) { & "$app\gesher-app.cmd" shutdown 2>&1 | Out-Null }
    if ($a.surfaceTrace) {
      $env:GESHER_SURFACE_TRACE_INVALIDATION = $null
      $env:GESHER_WINDOW_DEBUG = $null
      $info += 'mandatory default scenario: surface/window diagnostic flags unset'
    }
    if ($a.display) { $env:GESHER_DISPLAY = [string]$a.display; $info += "display $($a.display)" }
    if ($a.trace) {
      $traceDir = Join-Path $env:TEMP "gesher-e2e-trace-$pkg"
      New-Item -ItemType Directory -Force $traceDir | Out-Null
      $env:GESHER_APP_METHOD_TRACE = ($traceDir -replace '^[A-Za-z]:', '') -replace '\\', '/'
    }
    try {
      $ranWindow = $true
      $r = RunWindow $a "`"$app\gesher-app.cmd`" `"$apk`" --hidden-window" 'launch'
    } finally {
      if ($ownDevice) {
        & "$app\gesher-app.cmd" shutdown 2>&1 | Out-Null
        Remove-Item Env:GESHER_APP_METHOD_TRACE, Env:GESHER_DISPLAY -ErrorAction SilentlyContinue
      }
      $env:GESHER_SURFACE_TRACE_INVALIDATION = $surfaceTraceBefore
      $env:GESHER_WINDOW_DEBUG = $windowDebugBefore
      if ($traceDir) {
        Get-ChildItem $traceDir -Filter *.trace -ErrorAction SilentlyContinue | Copy-Item -Destination $OutDir
      }
    }
    $problems += $r.Problems
    $info += $r.Info
  }
  foreach ($l in $info) { Report "INFO  e2e: ${pkg}: $l" }
  $log = ReadText (Join-Path $OutDir "$pkg.launch.log")
  $appLog = AppLogPath $pkg
  if (Test-Path $appLog) { Copy-Item $appLog (Join-Path $OutDir "$pkg.app.log") -Force }
  # "expectLog": lines the launch's or the app's log must contain (e.g. that its WebView
  # got a WebView2, docs/webview.md).
  foreach ($want in @($a.expectLog | Where-Object { $_ })) {
    if (-not ($log + (ReadText $appLog)).Contains($want)) { $problems += "the log lacks '$want'" }
  }
  $standIns = [regex]::Matches($log + (ReadText $appLog), 'stand-in (\S+) not implemented') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
  if ($standIns) { Report "INFO  e2e: ${pkg}: stand-in methods called but not implemented: $($standIns -join ', ')" }
  $observedStatus = if (-not $problems) { 'passed' } elseif ($downloadFailed -or $identityFailed) { 'blocked' } else { 'failed' }
  Set-E2eCheck $pkg 'launch' $observedStatus $problems
  if (-not $problems) {
    if ($known) { Report "PASS  e2e: $name (listed as a known failure: $known)" }
    else { Report "PASS  e2e: $name installs, draws its first frame (not blank), reacts to input, closes and its process ends" }
    $passed += $a
  } else {
    $why = FirstProblem ((ReadText $appLog) + "`n" + $log)
    $line = "$name`: $($problems -join '; '). First problem in the logs: $why"
    if ($known) { Report "KNOWN-FAIL  e2e: $line (known failure: $known)" } else { Report "FAIL  e2e: $line"; $script:failed++ }
    # "diagnoseLog" (apps.json, optional): a regex; the logs' matching lines (up to 60)
    # are printed after a failure, for apps whose logs the summary would not show.
    if ($a.diagnoseLog) {
      (((ReadText $appLog) + "`n" + $log) -split "`n") | Where-Object { $_ -match $a.diagnoseLog } |
        Select-Object -Last 60 | ForEach-Object { Write-Host "INFO  e2e: ${pkg} log: $($_.TrimEnd())" }
    }
    if ($log.Contains('did not get ready')) {
      ((ReadText "$logs\system_server.log") -split "`n") | Select-Object -Last 60 |
        ForEach-Object { Write-Host "INFO  e2e: system server startup: $($_.TrimEnd())" }
    }
  }
  # Record the original default result and preserve its logs before running
  # instrumentation on a fresh device. Both scenarios are mandatory; a traced
  # pass cannot remove an original failure or replace its initial capture.
  if ($a.surfaceTrace -and $ranWindow) {
    Set-E2eCheck $pkg 'surface-trace' 'running'
    $surfaceTraceBefore = $env:GESHER_SURFACE_TRACE_INVALIDATION
    $windowDebugBefore = $env:GESHER_WINDOW_DEBUG
    $displayBefore = $env:GESHER_DISPLAY
    & "$app\gesher-app.cmd" shutdown 2>&1 | Out-Null
    $env:GESHER_SURFACE_TRACE_INVALIDATION = '1'
    $env:GESHER_WINDOW_DEBUG = '1'
    $env:GESHER_DISPLAY = [string]$a.display
    try {
      $traced = RunWindow $a "`"$app\gesher-app.cmd`" `"$apk`" --hidden-window" 'surface-trace'
    } finally {
      & "$app\gesher-app.cmd" shutdown 2>&1 | Out-Null
      $env:GESHER_SURFACE_TRACE_INVALIDATION = $surfaceTraceBefore
      $env:GESHER_WINDOW_DEBUG = $windowDebugBefore
      $env:GESHER_DISPLAY = $displayBefore
    }
    $tracedLog = ReadText (Join-Path $OutDir "$pkg.surface-trace.log")
    $tracedApp = ReadText $appLog
    if (Test-Path $appLog) { Copy-Item $appLog (Join-Path $OutDir "$pkg.surface-trace.app.log") -Force }
    foreach ($line in $traced.Info) { Report "INFO  e2e surface trace: ${pkg}: $line" }
    (($tracedApp + "`n" + $tracedLog) -split "`n") |
      Where-Object { $_ -match 'GesherSurface invalidation|Win32Surface:|EGL child' } |
      Select-Object -First 160 | ForEach-Object { Report "INFO  e2e surface trace: ${pkg}: $($_.TrimEnd())" }
    if ($traced.Problems.Count -eq 0) {
      Set-E2eCheck $pkg 'surface-trace' 'passed'
      Report "PASS  e2e surface trace: $pkg separately starts, draws, reacts to input and exits; original default result retained"
    } else {
      Set-E2eCheck $pkg 'surface-trace' 'failed' $traced.Problems
      Report "FAIL  e2e surface trace: ${pkg}: $($traced.Problems -join '; '). First problem: $(FirstProblem ($tracedApp + "`n" + $tracedLog))"
      $script:failed++
    }
  } elseif ($a.surfaceTrace) {
    Set-E2eCheck $pkg 'surface-trace' 'blocked' @() 'launch_not_reached'
  }
}

# --- The .apk association and the Start menu shortcut, for the first app that passed.
$first = $passed | Where-Object { -not $_.knownFailure } | Select-Object -First 1
if ($null -eq $first) { $first = $passed | Select-Object -First 1 }
if ($null -ne $first) {
  $pkg = $first.package
  $apk = Join-Path $apks "$($pkg)_$($first.versionCode).apk"
  $env:GESHER_APP_WINDOW = 'hidden-window'
  try {
    Set-E2eCheck $pkg 'association' 'running'
    $open = (Get-ItemProperty 'HKCU:\Software\Classes\Gesher.Apk\shell\open\command').'(default)'
    $r = RunWindow $first $open.Replace('%1', $apk) 'association' -NoInput
    foreach ($l in $r.Info) { Report "INFO  e2e: $pkg (.apk association): $l" }
    if ($r.Problems) { Report "FAIL  e2e: $pkg through the .apk association's command ($open): $($r.Problems -join '; ')"; $script:failed++ }
    else { Report "PASS  e2e: $pkg starts through the .apk association's registered command, draws, closes" }

    Set-E2eCheck $pkg 'association' $(if ($r.Problems) { 'failed' } else { 'passed' }) $r.Problems
    Set-E2eCheck $pkg 'shortcut' 'running'
    $lnk = Get-ChildItem (Join-Path ([Environment]::GetFolderPath('Programs')) 'Gesher Apps') -Filter *.lnk -ErrorAction SilentlyContinue |
      Where-Object { (New-Object -ComObject WScript.Shell).CreateShortcut($_.FullName).Arguments -match "launch $pkg" } | Select-Object -First 1
    if ($null -eq $lnk) {
      Report "FAIL  e2e: $pkg has no Start menu shortcut in Gesher Apps"; $script:failed++
      Set-E2eCheck $pkg 'shortcut' 'failed' @('no Start menu shortcut in Gesher Apps')
    }
    else {
      $link = (New-Object -ComObject WScript.Shell).CreateShortcut($lnk.FullName)
      $r = RunWindow $first "`"$($link.TargetPath)`" $($link.Arguments)" 'shortcut' -NoInput
      foreach ($l in $r.Info) { Report "INFO  e2e: $pkg (Start menu): $l" }
      if ($r.Problems) { Report "FAIL  e2e: $pkg through its Start menu shortcut '$($lnk.Name)': $($r.Problems -join '; ')"; $script:failed++ }
      else { Report "PASS  e2e: $pkg starts through its Start menu shortcut '$($lnk.BaseName)' (`"$($link.TargetPath)`" $($link.Arguments)), draws, closes" }
      Set-E2eCheck $pkg 'shortcut' $(if ($r.Problems) { 'failed' } else { 'passed' }) $r.Problems
    }
  } finally {
    Remove-Item Env:GESHER_APP_WINDOW -ErrorAction SilentlyContinue
  }
} else {
  Report 'FAIL  e2e: no app passed, so the .apk association and the Start menu shortcut were not tried'
  Set-E2eCheck '' 'association' 'blocked' @() 'no_successful_launch'
  Set-E2eCheck '' 'shortcut' 'blocked' @() 'no_successful_launch'
  $script:failed++
}

$logs = "$env:LOCALAPPDATA\Gesher\logs"
if (Test-Path $logs) { Copy-Item "$logs\system_server*.log" $OutDir -ErrorAction SilentlyContinue }
# What the apps asked the system server's stand-ins for that they do not answer yet.
$serverMisses = [regex]::Matches((ReadText "$logs\system_server.log"), 'stand-in (\S+) not implemented') |
  ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
Report "INFO  e2e: system_server stand-in methods called but not implemented: $(if ($serverMisses) { $serverMisses -join ', ' } else { 'none' })"
& "$app\gesher-app.cmd" shutdown 2>&1 | Out-Null
Report "INFO  e2e: $($script:failed) unexpected failures"
$script:E2eEvidence.completed = $true
Write-E2eEvidence
exit $(if ($script:failed) { 1 } else { 0 })
} finally {
  Close-E2eEvidenceRun
}
