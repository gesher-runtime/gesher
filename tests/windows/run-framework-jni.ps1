# Run only the software JNI contracts in separate, bounded candidate processes.
# BootDir/ProfileDir may point at an installed device, which is read-only here.
param(
  [Parameter(Mandatory)][string] $RuntimeExe,
  [Parameter(Mandatory)][string] $BootDir,
  [Parameter(Mandatory)][string] $ProfileDir,
  [Parameter(Mandatory)][string] $TestDex,
  [Parameter(Mandatory)][string] $OutputDir,
  [string[]] $Classes = @('gesher.FrameworkMemoryTest', 'gesher.FrameworkScryptTest',
                         'gesher.FrameworkLocalSocketTest', 'gesher.FrameworkCharsetTest')
)
$ErrorActionPreference = 'Stop'
function AndroidPath([string] $path) {
  (($path -replace '\\', '/') -replace '^[A-Za-z]:', '')
}
$RuntimeExe = (Resolve-Path -LiteralPath $RuntimeExe).Path
$BootDir = (Resolve-Path -LiteralPath $BootDir).Path
$ProfileDir = (Resolve-Path -LiteralPath $ProfileDir).Path
$TestDex = (Resolve-Path -LiteralPath $TestDex).Path
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$OutputDir = (Resolve-Path -LiteralPath $OutputDir).Path
$bcp = (@('core-oj', 'core-libart', 'core-icu4j', 'framework') |
  ForEach-Object { AndroidPath "$BootDir/$_.jar" }) -join ':'
$failures = 0
foreach ($class in $Classes) {
  $data = Join-Path $OutputDir "$class-data"
  New-Item -ItemType Directory -Force -Path $data | Out-Null
  $start = New-Object Diagnostics.ProcessStartInfo
  $start.FileName = $RuntimeExe
  $start.WorkingDirectory = Split-Path $RuntimeExe
  $vmArgs = @("-Xbootclasspath:$bcp", "-Ximage:$(AndroidPath "$BootDir/framework/boot.art")",
              '-Xint', '-Xusejit:false', '-cp', (AndroidPath $TestDex), $class)
  $start.Arguments = ($vmArgs | ForEach-Object { '"' + $_ + '"' }) -join ' '
  $start.UseShellExecute = $false
  $start.CreateNoWindow = $true
  $start.RedirectStandardOutput = $true
  $start.RedirectStandardError = $true
  $start.EnvironmentVariables['ANDROID_ROOT'] = AndroidPath "$ProfileDir/system"
  $start.EnvironmentVariables['ANDROID_DATA'] = AndroidPath $data
  $start.EnvironmentVariables['ANDROID_ART_ROOT'] = AndroidPath "$ProfileDir/art"
  $start.EnvironmentVariables['ANDROID_I18N_ROOT'] = AndroidPath "$ProfileDir/i18n"
  $start.EnvironmentVariables['ANDROID_TZDATA_ROOT'] = AndroidPath "$BootDir/tzdata"
  # An unset device cannot attach to the user's shared Binder endpoint.
  $start.EnvironmentVariables.Remove('GESHER_BINDER')
  $process = New-Object Diagnostics.Process
  $process.StartInfo = $start
  $null = $process.Start()
  $stdout = $process.StandardOutput.ReadToEndAsync()
  $stderr = $process.StandardError.ReadToEndAsync()
  $finished = $process.WaitForExit(30000)
  if (-not $finished) { $process.Kill(); $process.WaitForExit() }
  $output = $stdout.GetAwaiter().GetResult()
  $errorOutput = $stderr.GetAwaiter().GetResult()
  [IO.File]::WriteAllText((Join-Path $OutputDir "$class.stdout.log"), $output)
  [IO.File]::WriteAllText((Join-Path $OutputDir "$class.stderr.log"), $errorOutput)
  $passed = $finished -and $process.ExitCode -eq 0 -and $output.Contains('all passed')
  if ($passed) { Write-Host "PASS $class" }
  else {
    Write-Host "FAIL $class (finished=$finished, exit=$($process.ExitCode))"
    Write-Host $output
    Write-Host $errorOutput
    $failures++
  }
  $process.Dispose()
}
exit ([int]($failures -ne 0))
