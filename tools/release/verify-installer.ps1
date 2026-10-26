param(
  [Parameter(Mandatory)][string] $Directory,
  [ValidateSet('candidate','released')][string] $Phase = 'candidate',
  [string] $Logs = 'release-verification',
  [switch] $UninstallOnly
)
$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -cne 'true' -or $env:GITHUB_REPOSITORY -cne 'gesher-runtime/gesher') {
  throw 'Installer verification is restricted to the publishing CI runners'
}
$directoryPath = [IO.Path]::GetFullPath($Directory)
$logDirectory = [IO.Path]::GetFullPath($Logs)
[void][IO.Directory]::CreateDirectory($logDirectory)
$metadata = Get-Content -Raw -LiteralPath (Join-Path $directoryPath 'gesher-0.1.0-manifest.json') | ConvertFrom-Json
$setup = Join-Path $directoryPath 'gesher-setup-0.1.0.exe'
$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $setup).Hash.ToLowerInvariant()
if ($metadata.version -cne '0.1.0' -or $metadata.installer.sha256 -cne $hash -or
    (Get-Item -LiteralPath $setup).Length -ne $metadata.installer.size) { throw 'Installer does not match its tested receipt' }
$info = [Diagnostics.FileVersionInfo]::GetVersionInfo($setup)
if ($info.ProductName -cne 'Gesher' -or $info.ProductVersion -cne $metadata.installer.product_version -or
    $info.FileVersion -cne $metadata.installer.file_version -or
    $info.ProductVersion -notin @('0.1.0','0.1.0.0') -or $info.FileVersion -notin @('0.1.0','0.1.0.0')) {
  throw 'Downloaded installer PE metadata differs from the tested receipt'
}
function Run-ReleaseProcess([string] $Name, [string] $Exe, [string[]] $Arguments, [int] $Seconds = 180) {
  $start = [Diagnostics.ProcessStartInfo]::new($Exe)
  foreach ($argument in $Arguments) { $start.ArgumentList.Add($argument) }
  $start.UseShellExecute = $false
  $start.CreateNoWindow = $true
  $start.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
  $start.RedirectStandardOutput = $true
  $start.RedirectStandardError = $true
  $process = [Diagnostics.Process]::Start($start)
  $stdout = $process.StandardOutput.ReadToEndAsync()
  $stderr = $process.StandardError.ReadToEndAsync()
  if (-not $process.WaitForExit($Seconds * 1000)) {
    $process.Kill($true)
    $process.WaitForExit()
    throw "$Name timed out after $Seconds s"
  }
  if (-not $stdout.Wait(10000) -or -not $stderr.Wait(10000)) { throw "$Name left an output pipe open" }
  $text = $stdout.Result + $stderr.Result
  [IO.File]::WriteAllText((Join-Path $logDirectory "$Name.log"), $text)
  if ($process.ExitCode -ne 0) { throw "$Name failed with exit $($process.ExitCode): $text" }
  return $text
}
$local = [IO.Path]::GetFullPath($env:LOCALAPPDATA)
$app = [IO.Path]::GetFullPath((Join-Path $local 'Programs/Gesher'))
$data = [IO.Path]::GetFullPath((Join-Path $local 'Gesher'))
# These are the exact per-user locations owned by this installer. Validate them
# before installation/uninstallation can remove its files and device data.
if (-not $app.StartsWith($local + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
    -not $data.StartsWith($local + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
  throw 'Unexpected installer target paths'
}
if (-not $UninstallOnly -and (Test-Path -LiteralPath (Join-Path $app 'unins000.exe'))) {
  throw 'The clean verification runner already has Gesher installed'
}
$programs = [Environment]::GetFolderPath('Programs')
$sentinel = Join-Path $programs ('gesher-release-sentinel-' + [guid]::NewGuid() + '.txt')
$encoding = [Text.UTF8Encoding]::new($false)
[IO.File]::WriteAllText($sentinel, 'unrelated release verification fixture', $encoding)
$problems = [Collections.Generic.List[string]]::new()
$env:GESHER_REQUIRE_BOOT_IMAGE = '1'
try {
  if (-not $UninstallOnly) {
    $null = Run-ReleaseProcess 'install' $setup @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/SP-')
    $out = Run-ReleaseProcess 'hello' 'powershell.exe' @('-NoProfile','-ExecutionPolicy','Bypass','-File',"$app/gesher.ps1","$app/hello.dex",'gesher.Hello')
    if ($out -notmatch 'Hello from ART on Windows' -or $out -notmatch 'gesher: boot image loaded: 3 spaces') { throw 'Installed ART/boot-image Hello did not run' }
    $out = Run-ReleaseProcess 'lifecycle-single' 'powershell.exe' @('-NoProfile','-ExecutionPolicy','Bypass','-File',"$app/gesher-app.ps1","$app/lifecycle.apk",'--single-process')
    if ($out -notmatch 'LifecycleActivity onResume' -or $out -notmatch 'gesher-app: done' -or
        $out -notmatch 'gesher: boot image loaded: 5 spaces') { throw 'Installed framework lifecycle did not run' }
    $out = Run-ReleaseProcess 'lifecycle-device' 'powershell.exe' @('-NoProfile','-ExecutionPolicy','Bypass','-File',"$app/gesher-app.ps1","$app/lifecycle.apk",'--headless')
    if ($out -notmatch 'LifecycleActivity onResume' -or $out -notmatch 'gesher-app: done') { throw 'Installed device lifecycle did not run' }
    foreach ($required in 'gesher_arm64.dll','framework.jar','gesher-app.jar','libEGL.dll','libGLESv2.dll','licenses/NET-NOTICES.txt') {
      if (-not (Test-Path -LiteralPath (Join-Path $app $required))) { throw "Installed package omitted $required" }
    }
    Write-Host "PASS release $Phase bytes: PE version 0.1.0, SHA256 $hash; installed ART/boot image, framework and device lifecycle"
  }
} catch {
  $problems.Add($_.Exception.Message)
} finally {
  try {
    $runningDevice = @{}
    $deviceState = Join-Path $data 'data/system/gesher-device'
    if (Test-Path -LiteralPath $deviceState) {
      foreach ($line in Get-Content -LiteralPath $deviceState) {
        if ($line -match '^(system_server|servicemanager|binderd)=([0-9]+)$') {
          $deviceProcess = Get-Process -Id ([int]$Matches[2]) -ErrorAction SilentlyContinue
          if ($deviceProcess) { $runningDevice[$deviceProcess.Id] = $deviceProcess.StartTime }
        }
      }
    }
    $uninstaller = Join-Path $app 'unins000.exe' 
    if (Test-Path -LiteralPath $uninstaller) {
      $null = Run-ReleaseProcess 'uninstall' $uninstaller @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART')
      $deadline = [DateTime]::UtcNow.AddSeconds(60)
      while ((Test-Path -LiteralPath $uninstaller) -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 250 }
    }
    if (Test-Path -LiteralPath (Join-Path $app 'dalvikvm.exe')) { throw 'Uninstall left the runtime' }
    if (Test-Path -LiteralPath $data) { throw 'Uninstall left device data/process state' }
    foreach ($identifier in $runningDevice.Keys) {
      $deviceProcess = Get-Process -Id $identifier -ErrorAction SilentlyContinue
      if ($deviceProcess -and $deviceProcess.StartTime -eq $runningDevice[$identifier]) { throw 'Uninstall left a device process running' }
    }
    if (Test-Path -LiteralPath $uninstaller) { throw 'Uninstaller did not complete' }
    $pathValue = [Environment]::GetEnvironmentVariable('Path','User')
    if ($pathValue -split ';' -contains $app) { throw 'Uninstall left the user PATH entry' }
    if (Test-Path -LiteralPath 'HKCU:/Software/Classes/Gesher.Apk') { throw 'Uninstall left the APK ProgID' }
    if (Test-Path -LiteralPath 'HKCU:/Software/Classes/CLSID/{C18D9445-A7F0-472E-985F-AC354EBEB660}') { throw 'Uninstall left the toast activator' }
    $identities = @(Get-ChildItem 'HKCU:/Software/Classes/AppUserModelId' -ErrorAction SilentlyContinue | Where-Object PSChildName -like 'Gesher.App.*')
    if ($identities) { throw 'Uninstall left app identities' }
    if (Test-Path -LiteralPath (Join-Path $programs 'Gesher Apps')) { throw 'Uninstall left app shortcuts' }
    if (-not (Test-Path -LiteralPath $sentinel)) { throw 'Uninstall removed an unrelated Start menu fixture' }
    Write-Host "PASS release $Phase bytes: uninstall removes device data, runtime, PATH, APK/COM identities and app shortcuts; unrelated Start menu fixture retained"
  } catch { $problems.Add($_.Exception.Message) }
  if (Test-Path -LiteralPath $sentinel) { Remove-Item -LiteralPath $sentinel }
}
$receipt = [ordered] @{ phase = $Phase; installer_sha256 = $hash; source_sha = $metadata.source.sha
  successful = ($problems.Count -eq 0); uninstall_only = [bool]$UninstallOnly; problems = @($problems) }
[IO.File]::WriteAllText((Join-Path $logDirectory 'verification.json'), ($receipt | ConvertTo-Json -Depth 4) + "`n", $encoding)
if ($problems.Count) { throw ($problems -join '; ') }
