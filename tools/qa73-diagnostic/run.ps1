param([Parameter(Mandatory)][string]$Dir)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$Dir = (Resolve-Path -LiteralPath $Dir).Path
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$results = Join-Path $Dir 'results'
New-Item -ItemType Directory -Force $results | Out-Null
$results = (Resolve-Path -LiteralPath $results).Path
$controller = Join-Path $Dir 'pipe_job_runner.exe'
$historical = Join-Path $Dir 'historical'
$old = Join-Path $historical 'bin'
$rows = [Collections.Generic.List[object]]::new()
$problems = [Collections.Generic.List[string]]::new()

function Observe([string]$Name, [string]$Exe, [string[]]$Arguments, [int]$Milliseconds,
                 [string[]]$Required, [int[]]$Allowed) {
  $log = Join-Path $results "$Name.log"
  $summary = & $controller $Milliseconds $log $Exe @Arguments 2>&1
  $status = $LASTEXITCODE
  $summaryText = ($summary | Out-String)
  Set-Content -LiteralPath (Join-Path $results "$Name.controller.log") -Value $summaryText
  $text = if (Test-Path -LiteralPath $log) { [IO.File]::ReadAllText($log) } else { '' }
  $errors = [Collections.Generic.List[string]]::new()
  if ($summaryText -notmatch 'COMPONENT_JOB .* empty=1 ') { $errors.Add('Owned tree cleanup not proved') }
  if ($Allowed -notcontains $status) { $errors.Add("Unexpected diagnostic status $status") }
  foreach ($pattern in $Required) {
    if ($text -notmatch $pattern) { $errors.Add("Missing actual observation: $pattern") }
  }
  $rows.Add([ordered]@{name=$Name; executable=$Exe; executable_sha256=(Get-FileHash -LiteralPath $Exe).Hash.ToLowerInvariant()
    arguments=$Arguments; deadline_ms=$Milliseconds; status=$status; controller=$summaryText.Trim(); observations_complete=($errors.Count -eq 0)
    errors=$errors.ToArray(); log_sha256=$(if (Test-Path -LiteralPath $log) {(Get-FileHash -LiteralPath $log).Hash.ToLowerInvariant()}else{$null})})
  foreach ($problem in $errors) { $problems.Add("${Name}: $problem") }
  Write-Host "QA73_OBSERVATION name=$Name status=$status complete=$($errors.Count -eq 0)"
  Write-Host $summaryText
  Write-Host $text
  $rows | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $results 'observations.json')
}

# Preserve every original assertion and run the exact failed producer binaries
# alongside fresh, source-identical fixtures and observation-only variants.
Copy-Item -LiteralPath (Join-Path $historical 'historical-receipt.json') -Destination $results
Copy-Item -LiteralPath (Join-Path $Dir 'receipt.json') -Destination $results
$plain = Join-Path $Dir 'plain'; $observed = Join-Path $Dir 'observed'
New-Item -ItemType Directory -Force $plain,$observed | Out-Null
foreach ($destination in @($plain,$observed)) {
  Copy-Item -LiteralPath (Join-Path $old 'gesher-exec.exe') -Destination $destination
}
Copy-Item -LiteralPath (Join-Path $Dir 'ofd_spawn_original.exe') -Destination (Join-Path $plain 'ofd_spawn_test.exe')
Copy-Item -LiteralPath (Join-Path $Dir 'ofd_native_child.exe') -Destination $plain
Copy-Item -LiteralPath (Join-Path $Dir 'ofd_spawn_observed.exe') -Destination (Join-Path $observed 'ofd_spawn_test.exe')
Copy-Item -LiteralPath (Join-Path $Dir 'native_child_observed.exe') -Destination (Join-Path $observed 'ofd_native_child.exe')
Push-Location $results
try {
  Observe 'historical-stdio' (Join-Path $old 'stdio_ofd_test.exe') @() 120000 @('stdio OFD contracts: 175 checks, 1 failures','FAIL stdio: wide stdio flush uses current append status') @(1)
  Observe 'recompiled-stdio' (Join-Path $Dir 'stdio_ofd_test.exe') @() 120000 @('stdio OFD contracts: 175 checks, 1 failures','FAIL stdio: wide stdio flush uses current append status') @(1)
  Observe 'observed-stdio' (Join-Path $Dir 'stdio_ofd_observed.exe') @() 120000 @('stdio OFD contracts: 175 checks, 1 failures','QA73 WIDE fflush=') @(1)
  Observe 'historical-spawn' (Join-Path $old 'ofd_spawn_test.exe') @() 120000 @('OFD Windows spawn: 33 checks, 1 failures','native child sees closed standard handles') @(1)
  Observe 'recompiled-spawn' (Join-Path $plain 'ofd_spawn_test.exe') @() 120000 @('OFD Windows spawn: 33 checks, 1 failures','native child sees closed standard handles') @(1)
  $env:GESHER_QA73_CHILD_LOG = Join-Path $results 'native-child-handles.log'
  try {
    Observe 'observed-spawn' (Join-Path $observed 'ofd_spawn_test.exe') @() 120000 @('OFD Windows spawn: 33 checks, 1 failures','QA73 WAIT .* code=') @(1)
  } finally { Remove-Item Env:GESHER_QA73_CHILD_LOG }
  if (-not (Test-Path -LiteralPath (Join-Path $results 'native-child-handles.log'))) { $problems.Add('No actual native child handle report') }
  else {
    $handles = [IO.File]::ReadAllText((Join-Path $results 'native-child-handles.log'))
    Write-Host $handles
    foreach ($fd in 0..2) {
      if ($handles -notmatch "QA73 CHILD .* mode=--empty fd=$fd ") { $problems.Add("Missing empty-table child descriptor $fd observation") }
    }
  }

  # Use the exact unmodified APK pinned by the original e2e workflow. Extract
  # its own x86_64 library set; do not substitute an ELF fixture for the app.
  . (Join-Path $root 'tests/e2e/apks.ps1')
  $manifest = Get-Content -Raw (Join-Path $root 'tests/e2e/apps.json') | ConvertFrom-Json
  $app = @($manifest.apps | Where-Object { $_.package -eq 'name.boyle.chris.sgtpuzzles' })
  if ($app.Count -ne 1 -or $app[0].versionCode -ne 149 -or $app[0].sha256 -cne '6b36d5537984523c57984a784156eb853a92c940fded286483687484f3d652e4') { throw 'Pinned original Puzzles manifest changed' }
  $apk = Join-Path $Dir 'name.boyle.chris.sgtpuzzles_149.apk'
  Get-PinnedApk $apk 'name.boyle.chris.sgtpuzzles_149.apk' $app[0].sha256 (@(@($manifest.repo) + @($manifest.mirrors)) | Where-Object { $_ })
  $libs = Join-Path $Dir 'puzzles-libs'; New-Item -ItemType Directory -Force $libs | Out-Null
  $archive = [IO.Compression.ZipFile]::OpenRead($apk)
  try {
    $members = @()
    foreach ($entry in $archive.Entries) {
      if ($entry.FullName -match '^lib/x86_64/([^/]+\.so)$') {
        $path = Join-Path $libs $Matches[1]
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry,$path,$true)
        $members += [ordered]@{member=$entry.FullName; sha256=(Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant()}
      }
    }
  } finally { $archive.Dispose() }
  [ordered]@{apk_sha256=(Get-FileHash -LiteralPath $apk).Hash.ToLowerInvariant(); source_manifest_sha256=(Get-FileHash (Join-Path $root 'tests/e2e/apps.json')).Hash.ToLowerInvariant(); members=$members} | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $results 'puzzles-apk-receipt.json')
  $elf = Join-Path $libs 'libpuzzlesgen.so'
  if ((Get-FileHash $elf).Hash.ToLowerInvariant() -cne 'ddcb9d0e49c4794c821c194280b141ebbe0cf3cbca77960bc6a470ca590b7fb9') { throw 'Generator member mismatch' }
  $launcher = Join-Path $old 'gesher-exec.exe'
  Observe 'puzzles-direct-native-stdio' $launcher @('--elf',$elf,$elf,'keen','6dn') 120000 @() @(0,1,124,127)
  try {
    $env:GESHER_QA73_PIPE_REPORT_PREFIX = Join-Path $results 'real-pipe-tool'
    Observe 'real-pipe-tool' (Join-Path $Dir 'puzzles_pipe_probe.exe') @('tool',$launcher,$elf) 130000 @('QA73 PUZZLES_RESULT mode=tool','QA73 CHILD_STDERR_BEGIN') @(0,1,124,127)
    $env:GESHER_QA73_PIPE_REPORT_PREFIX = Join-Path $results 'real-pipe-puzzles'
    Observe 'real-pipe-puzzles' (Join-Path $Dir 'puzzles_pipe_probe.exe') @('puzzles',$launcher,$elf) 130000 @('QA73 PUZZLES_RESULT mode=puzzles','QA73 CHILD_STDERR_BEGIN') @(0,1,124,127)
  } finally { Remove-Item Env:GESHER_QA73_PIPE_REPORT_PREFIX -ErrorAction SilentlyContinue }

  # The real fixture stays untouched. The observer uses redirected pipes and
  # samples only the process it started; a timeout remains a timeout.
  Observe 'historical-binder-file-output' (Join-Path $old 'libbinder_probe.exe') @() 120000 @('libbinder probe: all passed') @(0,124)
  Observe 'historical-binder-pipe-observer' (Join-Path $Dir 'binder_observer.exe') @((Join-Path $old 'libbinder_probe.exe')) 130000 @('libbinder probe: all passed','QA73 BINDER_RESULT') @(0,124)
} finally {
  Pop-Location
  [ordered]@{purpose='Diagnosis of known failed original binaries; completed observations are not product QA approval'; source=$env:GITHUB_SHA; run_id=$env:GITHUB_RUN_ID; attempt=$env:GITHUB_RUN_ATTEMPT; rows=$rows.ToArray(); errors=$problems.ToArray()} | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $results 'summary.json')
}
if ($problems.Count) { throw ($problems -join "`n") }
Write-Host 'QA73_DIAGNOSTIC_OBSERVATIONS_COMPLETE (original product failures remain failures)'
