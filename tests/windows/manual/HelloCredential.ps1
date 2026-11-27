# Run the device-credential APK against a disposable profile of an exact packaged runtime.
[CmdletBinding()]
param(
  [Parameter(Mandatory)][string]$RuntimeDirectory,
  [Parameter(Mandatory)][string]$Apk,
  [Parameter(Mandatory)][ValidatePattern('^[0-9a-fA-F]{40}$')][string]$SourceRevision,
  [ValidateSet('all', 'verify', 'user-cancel', 'app-cancel', 'owner-lost')][string]$Scenario = 'all',
  [string]$ScratchDirectory = [IO.Path]::GetTempPath(),
  [ValidateRange(30, 300)][int]$TimeoutSeconds = 120
)
$ErrorActionPreference = 'Stop'
$runtime = (Resolve-Path -LiteralPath $RuntimeDirectory).Path
$fixture = (Resolve-Path -LiteralPath $Apk).Path
$scratchParent = (Resolve-Path -LiteralPath $ScratchDirectory).Path
if ([IO.Path]::GetPathRoot($runtime) -ne [IO.Path]::GetPathRoot($scratchParent)) {
  throw 'Choose a disposable scratch directory on the same drive as the runtime.'
}
$launcher = Join-Path $runtime 'gesher-app.ps1'
foreach ($name in 'gesher-app.ps1', 'dalvikvm.exe', 'gesher-app.jar') {
  if (!(Test-Path -LiteralPath (Join-Path $runtime $name) -PathType Leaf)) {
    throw 'RuntimeDirectory must be the App directory of the verified package.'
  }
}

if (-not ('GesherHelloFixtureToken' -as [type])) {
  Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class GesherHelloFixtureToken {
  [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(int access, bool inherit, int pid);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
  [DllImport("advapi32.dll")] static extern bool OpenProcessToken(IntPtr process, int access, out IntPtr token);
  [DllImport("advapi32.dll")] static extern bool GetTokenInformation(IntPtr token, int info, out int value, int size, out int returned);
  public static bool IsAppContainer(int pid) {
    IntPtr process = OpenProcess(0x1000, false, pid);
    if (process == IntPtr.Zero) return false;
    try {
      IntPtr token;
      if (!OpenProcessToken(process, 8, out token)) return false;
      try {
        int value, returned;
        return GetTokenInformation(token, 29, out value, 4, out returned) && value != 0;
      } finally { CloseHandle(token); }
    } finally { CloseHandle(process); }
  }
}
"@
}

function Quote-Arg([string]$Value) {
  '"' + ($Value -replace '(\*)"', '$1$1"' -replace '(\+)$', '$1$1') + '"'
}

function Invoke-Launcher([string]$Profile, [string[]]$Command, [int]$Seconds, [string]$AppLog = '') {
  $powershell = (Get-Process -Id $PID).Path
  $argv = @('-NoProfile', '-File', $launcher) + $Command
  $info = [Diagnostics.ProcessStartInfo]::new($powershell, (($argv | ForEach-Object { Quote-Arg $_ }) -join ' '))
  $info.WorkingDirectory = $runtime
  $info.UseShellExecute = $false
  $info.RedirectStandardOutput = $true
  $info.RedirectStandardError = $true
  $info.EnvironmentVariables['GESHER_PROFILE'] = $Profile
  $info.EnvironmentVariables['GESHER_SHORTCUTS'] = '0'
  $info.EnvironmentVariables['GESHER_MICROG_SPOOF'] = '0'
  $info.EnvironmentVariables['GESHER_REQUIRE_BOOT_IMAGE'] = '1'
  $info.EnvironmentVariables['GESHER_IDLE_TIMEOUT'] = '30'
  # Use the package's real default. An inherited developer mode would invalidate this run.
  foreach ($key in 'GESHER_SANDBOX', 'GESHER_BINDER', 'GESHER_HELLOCRED_MODE') {
    $info.EnvironmentVariables.Remove($key)
  }
  $process = [Diagnostics.Process]::Start($info)
  try {
    $null = $process.StandardOutput.ReadToEndAsync()
    $null = $process.StandardError.ReadToEndAsync()
    $contained = $false
    $deadline = [Diagnostics.Stopwatch]::StartNew()
    while (!$process.WaitForExit(100) -and $deadline.Elapsed.TotalSeconds -lt $Seconds) {
      if (!$contained -and $AppLog) {
        $text = Get-Content -LiteralPath $AppLog -Raw -ErrorAction SilentlyContinue
        if ($text -match '(?m)^hellocred: probe-process: ([1-9][0-9]*)\s*$') {
          $contained = [GesherHelloFixtureToken]::IsAppContainer([int]$Matches[1])
        }
      }
    }
    if (!$process.HasExited) {
      $process.Kill()
      $null = $process.WaitForExit(5000)
      return @{ Completed = $false; AppContainer = $contained; ExitCode = -1 }
    }
    return @{ Completed = $true; AppContainer = $contained; ExitCode = $process.ExitCode }
  } finally { $process.Dispose() }
}

function Check-Scenario([string]$Run, [string]$Text, [string]$Server, [bool]$Contained) {
  $errors = [Collections.Generic.List[string]]::new()
  if (!$Contained) { $errors.Add('Windows AppContainer token was not observed') }
  if ($Text -notmatch "(?m)^hellocred: mode: $Run\s*$" -or
      $Text -notmatch '(?m)^hellocred: done\s*$') { $errors.Add('fixture did not complete the selected scenario') }
  if ($Text -notmatch 'feature: secure_lock_screen true' -or
      $Text -notmatch 'keyguard: isDeviceSecure true, isKeyguardSecure true' -or
      $Text -notmatch 'biometrics: canAuthenticate credential 0, strong 12, weak 12, strong\|credential 0, weak\|credential 0') {
    $errors.Add('enrolled desktop credential capability was not observed')
  }
  if ($Text -notmatch 'operation-bound: error 1' -or $Text -match 'operation-bound: unexpected success') {
    $errors.Add('operation-bound Binder request did not return HW_UNAVAILABLE')
  }
  if ($Text -notmatch 'prompt: strong: error 12\b' -or $Text -match 'prompt: strong: succeeded') {
    $errors.Add('biometric-only request did not return HW_NOT_PRESENT')
  }
  if ($Run -eq 'verify') {
    if ($Text -notmatch 'prompt: credential: succeeded, type 1, crypto null' -or
        $Text -notmatch 'confirm: result RESULT_OK') {
      $errors.Add('BiometricPrompt or Keyguard credential confirmation did not succeed')
    }
  } else {
    $code = if ($Run -eq 'app-cancel') { 5 } else { 10 }
    if ($Text -notmatch "prompt: credential: error $code\b" -or $Text -match 'prompt: credential: succeeded|confirm: result RESULT_OK') {
      $errors.Add('cancellation did not return the expected Android error')
    }
    if ($Run -eq 'user-cancel' -and $Text -notmatch 'confirm: result RESULT_CANCELED') {
      $errors.Add('the Keyguard confirmation intent did not report cancellation')
    }
    if ($Run -eq 'app-cancel' -and $Text -notmatch 'prompt: credential: the app cancels') {
      $errors.Add('the application did not request cancellation')
    }
    if ($Run -eq 'owner-lost' -and $Server -notmatch "the owner of Windows Hello's prompt \(pid [0-9]+\) left the foreground: canceling") {
      $errors.Add('the verifier did not record foreground-owner loss')
    }
  }
  return $errors.ToArray()
}

$runs = if ($Scenario -eq 'all') { 'verify', 'user-cancel', 'app-cancel', 'owner-lost' } else { $Scenario }
$instructions = @{
  'verify' = 'When the fixture window opens, focus it if needed. Complete both Windows Hello prompts.'
  'user-cancel' = 'Focus the fixture if needed. Choose Cancel in both Windows Hello prompts.'
  'app-cancel' = 'Focus the fixture if needed. Observe the Windows Hello prompt and leave it untouched while the app cancels.'
  'owner-lost' = 'Wait for the Windows Hello prompt, then switch to an unrelated window and stay there until the prompt closes.'
}
$caseRoot = Join-Path $scratchParent ('Gesher-Hello-Verification-' + [Guid]::NewGuid().ToString('N'))
New-Item -Path $caseRoot -ItemType Directory | Out-Null
$testApk = Join-Path $caseRoot 'hellocred.apk'
Copy-Item -LiteralPath $fixture -Destination $testApk
Write-Host "Claimed source revision: $SourceRevision"
foreach ($item in @{ 'APK' = $testApk; 'VM' = (Join-Path $runtime 'dalvikvm.exe');
    'app jar' = (Join-Path $runtime 'gesher-app.jar') }.GetEnumerator()) {
  Write-Host "$($item.Key) SHA256 $((Get-FileHash -LiteralPath $item.Value -Algorithm SHA256).Hash)"
}
$failed = $false
$clean = $true
foreach ($run in $runs) {
  $profile = Join-Path $caseRoot $run
  $log = Join-Path $profile 'logs/com.gesher.hellocred.log'
  $serverLog = Join-Path $profile 'logs/system_server.log'
  $arguments = @('launch', $testApk, '--window', '--keep-task', '--wait', '--ez', 'manual-hello', 'true')
  if ($run -ne 'verify') { $arguments += @('--ez', $run, 'true') }
  Write-Host "${run}: $($instructions[$run])"
  try {
    $result = Invoke-Launcher -Profile $profile -Command $arguments -Seconds $TimeoutSeconds -AppLog $log
    if (!$result.Completed -or $result.ExitCode -ne 0) {
      $failed = $true
      Write-Host "FAIL $run - the fixture did not exit successfully before the deadline"
    } else {
      $text = [string](Get-Content -LiteralPath $log -Raw -ErrorAction SilentlyContinue)
      $server = [string](Get-Content -LiteralPath $serverLog -Raw -ErrorAction SilentlyContinue)
      $problems = @(Check-Scenario -Run $run -Text $text -Server $server -Contained $result.AppContainer)
      if ($problems.Count) {
        $failed = $true
        Write-Host "FAIL $run - $($problems -join ', ')"
      } else {
        Write-Host "PASS $run"
      }
    }
  } finally {
    if (Test-Path -LiteralPath $profile) {
      $stop = Invoke-Launcher -Profile $profile -Command @('shutdown') -Seconds 30
      if (!$stop.Completed -or $stop.ExitCode -ne 0) {
        $clean = $false
        $failed = $true
        Write-Host "FAIL $run - the disposable device did not acknowledge shutdown"
      }
    }
  }
}
# Keep raw diagnostics to this local temp directory only if the device may still be using them.
if ($clean) {
  $resolved = (Resolve-Path -LiteralPath $caseRoot).Path
  if ([IO.Path]::GetDirectoryName($resolved) -eq $scratchParent -and
      [IO.Path]::GetFileName($resolved) -match '^Gesher-Hello-Verification-[0-9a-f]{32}$') {
    Remove-Item -LiteralPath $resolved -Recurse -Force
  }
}
if ($failed) { exit 1 }
