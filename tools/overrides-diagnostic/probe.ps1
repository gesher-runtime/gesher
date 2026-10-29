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
  $requestedCwd = if ($Mode -eq 'isolated') { $work } else { $root }
  foreach ($path in @($wrapper, $probe, $operation)) { if ($path.Contains('"')) { throw 'Unsupported diagnostic argument path' } }
  Add-Type -Path (Join-Path $PSScriptRoot 'GeocoderCwdSupervisor.cs')
  [GeocoderCwdSupervisor]::DiagnosticWorkingDirectory = $requestedCwd
  $args = '-NoProfile -ExecutionPolicy Bypass -File "' + $wrapper + '" "' + $probe + '" "' + $operation + '"'
  $result = [GeocoderCwdSupervisor]::Run($exe, $args, 45000, '')
  Assert-GeocoderOwnedJob $result $exe 'cwd-diagnostic-provider'
  $text = $result.Output + $result.Error
  Write-Host "GEOCODER_PROVIDER mode=$Mode current_run=$actualRun source=$source pid=$($result.Pid) birth=$($result.Birth) exit=$($result.ExitCode) requested_cwd=$requestedCwd probe_sha256=$((Get-FileHash -LiteralPath $probe -Algorithm SHA256).Hash) exe_sha256=$((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash)"
  Write-Host $text
  $cwdMarker = 'GEOCODER_CHILD_CWD pid=' + $result.Pid + ' native=' + $requestedCwd + ' shell='
  if ($text.IndexOf($cwdMarker, [StringComparison]::OrdinalIgnoreCase) -lt 0 -or $text -notmatch 'status: ([A-Za-z]+)' -or $text -notmatch 'WINRT_PROVIDER task_status=RanToCompletion') { throw 'Original provider result or actual native working directory missing' }
  Observe 'after-provider'
} finally {
  Remove-GeocoderProbeAssembly
  Pop-Location
  $resolvedWork = [IO.Path]::GetFullPath($work)
  $tempPrefix = [IO.Path]::GetFullPath($env:RUNNER_TEMP).TrimEnd('\') + '\'
  if (-not $resolvedWork.StartsWith($tempPrefix, [StringComparison]::OrdinalIgnoreCase) -or
      [IO.Path]::GetFileName($resolvedWork) -notmatch '^geocoder-cwd-[0-9a-f]{32}$') { throw 'Invalid diagnostic cleanup target' }
  Remove-Item -LiteralPath $resolvedWork -Recurse -Force
}
