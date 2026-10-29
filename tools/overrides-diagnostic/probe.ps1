param([Parameter(Mandatory)][string] $MeasuredRoot, [Parameter(Mandatory)][ValidateSet('original','isolated')][string] $Mode)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $MeasuredRoot).Path
$source = 'fad0eff2f73215235769566b4c0304c844598195'
if ((git -C $root rev-parse HEAD).Trim() -cne $source) { throw 'Measured source mismatch' }
$actualRun = $env:GITHUB_RUN_ID
$env:GITHUB_SHA = $source
$work = Join-Path $env:RUNNER_TEMP ('geocoder-cwd-' + [guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($work)
function Observe([string] $phase) {
  $blob = (git -C $root hash-object -- overrides.json).Trim()
  $expected = (git -C $root rev-parse HEAD:overrides.json).Trim()
  $raw = (Get-FileHash -LiteralPath (Join-Path $root 'overrides.json') -Algorithm SHA256).Hash.ToLowerInvariant()
  $tempPath = Join-Path $work 'overrides.json'
  $tempRaw = if (Test-Path -LiteralPath $tempPath -PathType Leaf) { (Get-FileHash -LiteralPath $tempPath -Algorithm SHA256).Hash.ToLowerInvariant() } else { 'absent' }
  Write-Host "GEOCODER_SOURCE mode=$Mode phase=$phase blob=$blob expected=$expected raw_sha256=$raw isolated_raw_sha256=$tempRaw"
}
Push-Location $root
try {
  . (Join-Path $root 'tests/windows/helpers/geocoder-compile.ps1')
  Observe 'before-compiler'
  New-GeocoderProbeAssembly
  Observe 'after-compiler'
  $exe = Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe'
  $probe = Join-Path $root 'tests/windows/helpers/geocoder-provider-probe.ps1'
  $operation = Join-Path $root 'tests/windows/tracks/geocoder-operation.ps1'
  $wrapper = Join-Path $work 'provider-wrapper.ps1'
  @'
param([string] $Probe, [string] $Helper)
[Console]::Out.WriteLine('GEOCODER_CHILD_CWD pid=' + $PID + ' native=' + [Environment]::CurrentDirectory + ' shell=' + (Get-Location).Path)
[Console]::Out.Flush()
& $Probe $Helper
'@ | Set-Content -LiteralPath $wrapper -Encoding utf8
  $info = [Diagnostics.ProcessStartInfo]::new($exe)
  foreach ($arg in @('-NoProfile','-ExecutionPolicy','Bypass','-File',$wrapper,$probe,$operation)) { $info.ArgumentList.Add($arg) }
  $info.UseShellExecute = $false
  $info.RedirectStandardOutput = $true
  $info.RedirectStandardError = $true
  $info.WorkingDirectory = if ($Mode -eq 'isolated') { $work } else { $root }
  $process = [Diagnostics.Process]::Start($info)
  if ($null -eq $process) { throw 'Provider process not created' }
  $pidValue = $process.Id
  $birth = $process.StartTime.ToUniversalTime().ToString('o')
  $stdout = $process.StandardOutput.ReadToEndAsync(); $stderr = $process.StandardError.ReadToEndAsync()
  try {
    if (-not $process.WaitForExit(45000)) {
      try { $process.Kill($true) } catch { }
      if (-not $process.WaitForExit(5000)) { [Environment]::FailFast('Provider termination not proved') }
      throw 'Actual provider timed out'
    }
    if (-not [Threading.Tasks.Task]::WaitAll([Threading.Tasks.Task[]]@($stdout,$stderr),5000)) { throw 'Provider output drain failed' }
    $text = $stdout.Result + $stderr.Result
    Write-Host "GEOCODER_PROVIDER mode=$Mode current_run=$actualRun source=$source pid=$pidValue birth=$birth exit=$($process.ExitCode) requested_cwd=$($info.WorkingDirectory) probe_sha256=$((Get-FileHash -LiteralPath $probe -Algorithm SHA256).Hash) exe_sha256=$((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash)"
    Write-Host $text
    $cwdMarker = 'GEOCODER_CHILD_CWD pid=' + $pidValue + ' native=' + $info.WorkingDirectory + ' shell='
    if ($process.ExitCode -ne 0 -or $text.IndexOf($cwdMarker, [StringComparison]::OrdinalIgnoreCase) -lt 0 -or $text -notmatch 'status: ([A-Za-z]+)' -or $text -notmatch 'WINRT_PROVIDER task_status=RanToCompletion') { throw 'Original provider result or actual native working directory missing' }
  } finally { $process.Dispose() }
  Observe 'after-provider'
} finally {
  Remove-GeocoderProbeAssembly
  Pop-Location
  Remove-Item -LiteralPath $work -Recurse -Force
}
