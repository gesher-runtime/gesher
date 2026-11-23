$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'SplashReceipts.ps1')
$script:ReceiptChecks = 0
function Assert-Receipt([bool] $Condition, [string] $Message) {
  if (-not $Condition) { throw $Message }
  $script:ReceiptChecks++
}

$work = Join-Path ([IO.Path]::GetTempPath()) ('splash receipts $ & ' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($work) | Out-Null
$reader = $null
$worker = $null
$owned = $null
try {
  $parent = Join-Path $work 'parent.txt'
  $child = Join-Path $work 'child.txt'
  Write-SplashReceipt -Path $parent -Lines @('splash=True', 'title-ok=True')
  $before = [IO.File]::ReadAllText($parent)
  # Hold the reader throughout publication. This is the conflicting access in
  # the old Get-Content + Windows PowerShell Add-Content completion protocol.
  $reader = [IO.File]::Open($parent, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
  if ($PSVersionTable.PSVersion.Major -eq 5) {
    $blocked = $false
    try { Add-Content -LiteralPath $parent -Encoding ascii -Value 'old-completion' -ErrorAction Stop }
    catch [IO.IOException] {
      $blocked = $true
      Write-Host "SPLASH_LEGACY_APPEND_ERROR=$($_.FullyQualifiedErrorId): $($_.Exception.Message)"
    }
    catch {
      if ($_.FullyQualifiedErrorId -notmatch 'GetContentWriter|WriteContent') { throw }
      $blocked = $true
      Write-Host "SPLASH_LEGACY_APPEND_ERROR=$($_.FullyQualifiedErrorId): $($_.Exception.Message)"
    }
    # Diagnostic negative control, not a prerequisite on hosts where Microsoft
    # might backport the FileShare.ReadWrite fix. The new protocol never appends.
    Write-Host "SPLASH_LEGACY_APPEND_READER_BLOCKED=$blocked"
    if ($blocked) { Assert-Receipt ([IO.File]::ReadAllText($parent) -ceq $before) 'Failed legacy append changed the parent receipt.' }
    else { $before = [IO.File]::ReadAllText($parent) }
  }
  $expected = @('splash-after-exit=True', 'set-aumid=0', 'gone=True', 'app-closed=0/1', 'child-done=True')
  Write-SplashReceipt -Path $child -Lines $expected
  Assert-Receipt ([IO.File]::ReadAllText($parent) -ceq $before) 'Child publication modified the parent receipt.'
  Assert-Receipt (([IO.File]::ReadAllLines($child) -join '|') -ceq ($expected -join '|')) 'Child receipt was incomplete or changed.'
  Assert-Receipt (@([IO.Directory]::GetFiles($work, '*.tmp')).Count -eq 0) 'Publication left a temporary receipt.'
  $rejected = $false
  try { Write-SplashReceipt -Path $child -Lines @('wrong case') } catch { $rejected = $true }
  Assert-Receipt $rejected 'A second writer silently overwrote an immutable receipt.'
  Assert-Receipt (([IO.File]::ReadAllLines($child) -join '|') -ceq ($expected -join '|')) 'Duplicate publication corrupted the first receipt.'
  Assert-Receipt (@([IO.Directory]::GetFiles($work, '*.tmp')).Count -eq 0) 'Rejected publication leaked its temporary file.'
  $reader.Dispose(); $reader = $null

  $workerScript = Join-Path $work 'owned child.ps1'
  $ready = Join-Path $work 'ready'
  $stop = Join-Path $work 'stop'
  [IO.File]::WriteAllText($workerScript, @'
param([string] $Ready, [string] $Stop)
$ErrorActionPreference = 'Stop'
[IO.File]::WriteAllText($Ready, 'ready')
$deadline = [Diagnostics.Stopwatch]::StartNew()
while (-not [IO.File]::Exists($Stop)) {
  if ($deadline.ElapsedMilliseconds -gt 30000) { exit 25 }
  Start-Sleep -Milliseconds 20
}
'@)
  $hostPath = [Diagnostics.Process]::GetCurrentProcess().MainModule.FileName
  $arguments = '-NoProfile -NonInteractive -File "' + $workerScript + '" "' + $ready + '" "' + $stop + '"'
  $worker = Start-Process -FilePath $hostPath -ArgumentList $arguments -PassThru
  $null = $worker.Handle
  $birth = $worker.StartTime.ToFileTimeUtc()
  $watch = [Diagnostics.Stopwatch]::StartNew()
  while (-not [IO.File]::Exists($ready) -and $watch.ElapsedMilliseconds -lt 10000) {
    if ($worker.HasExited) { throw "Receipt fixture child exited $($worker.ExitCode) before ready." }
    Start-Sleep -Milliseconds 20
  }
  Assert-Receipt ([IO.File]::Exists($ready)) 'Receipt fixture child did not start.'
  $identity = @("child-pid=$($worker.Id)", "child-start=$birth")
  $owned = Get-SplashChildProcess -ParentReceipt $identity
  Assert-Receipt ($null -ne $owned -and $owned.Id -eq $worker.Id) 'Could not retain the recorded child process.'
  $wrong = Get-SplashChildProcess -ParentReceipt @("child-pid=$($worker.Id)", "child-start=$($birth + 1)")
  Assert-Receipt ($null -eq $wrong) 'Accepted a different process generation.'
  Assert-Receipt (-not $worker.HasExited) 'Generation rejection affected the running child.'
  $bad = $false
  try { $null = Get-SplashChildProcess -ParentReceipt @("child-pid=$($worker.Id)") } catch { $bad = $true }
  Assert-Receipt $bad 'Missing creation-time identity was accepted.'
  $bad = $false
  try { $null = Get-SplashChildProcess -ParentReceipt ($identity + "child-pid=$($worker.Id)") } catch { $bad = $true }
  Assert-Receipt $bad 'Ambiguous process identity was accepted.'
  [IO.File]::WriteAllText($stop, 'stop')
  Assert-Receipt ($owned.WaitForExit(5000)) 'Owned process did not retire after its gate opened.'
  Assert-Receipt ($owned.ExitCode -eq 0) 'Owned process did not complete normally.'
  $after = Get-SplashChildProcess -ParentReceipt $identity
  Assert-Receipt ($null -eq $after) 'Retired process was returned as a live owned child.'
  Write-Host "PASS  splash receipts: immutable publication under a held reader and owned process retirement ($script:ReceiptChecks checks)"
} finally {
  if ($reader) { $reader.Dispose() }
  if ($owned) { $owned.Dispose() }
  if ($worker) {
    try {
      if (-not $worker.HasExited) { $worker.Kill(); $null = $worker.WaitForExit(5000) }
    } finally { $worker.Dispose() }
  }
  [IO.Directory]::Delete($work, $true)
}
