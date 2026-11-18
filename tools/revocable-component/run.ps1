param([Parameter(Mandatory)][string]$Dir)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$Dir = (Resolve-Path -LiteralPath $Dir).Path
$receipt = Get-Content -LiteralPath (Join-Path $Dir 'receipt.json') -Raw | ConvertFrom-Json
$head = (git rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $receipt.schema -ne 1 -or $receipt.dirty -or
    $receipt.head -cne $head -or $head -cne $env:GITHUB_SHA -or
    $receipt.run_id -cne $env:GITHUB_RUN_ID -or $receipt.run_attempt -cne $env:GITHUB_RUN_ATTEMPT) {
    throw 'Component artifact does not identify this clean workflow commit and attempt'
}
foreach ($entry in $receipt.sources.PSObject.Properties) {
    $hash = (Get-FileHash -LiteralPath $entry.Name -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($hash -cne $entry.Value) { throw "Source changed after component build: $($entry.Name)" }
}
foreach ($entry in $receipt.binaries.PSObject.Properties) {
    $hash = (Get-FileHash -LiteralPath (Join-Path $Dir $entry.Name) -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($hash -cne $entry.Value) { throw "Component executable hash mismatch: $($entry.Name)" }
}
$runner = Join-Path $Dir 'revocable_job_runner.exe'
$logs = Join-Path $PWD 'component-logs'
[void][IO.Directory]::CreateDirectory($logs)
$pwsh = (Get-Command pwsh).Source
function Run-Owned([string]$Name, [int]$Timeout, [string]$Executable, [string[]]$Arguments, [int]$Expected, [string]$Marker) {
    $log = Join-Path $logs ($Name + '.log')
    $controller = & $runner ([string]$Timeout) $log $Executable @Arguments 2>&1
    $code = $LASTEXITCODE
    $controllerText = $controller -join "`n"
    [IO.File]::WriteAllText((Join-Path $logs ($Name + '.controller.log')), $controllerText)
    Write-Host $controllerText
    $text = Get-Content -LiteralPath $log -Raw
    Write-Host $text
    if ($code -ne $Expected -or $controllerText -notmatch 'empty=1' -or $text -notmatch $Marker) {
        throw "Owned fixture failed: $Name exit=$code expected=$Expected"
    }
    return $controllerText
}
# Positive, nonzero, and real descendant timeout controls qualify cleanup
# before it is relied on for the production fixtures.
$positiveProgram = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes("Write-Output 'CONTROL positive'; exit 0"))
$nonzeroProgram = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes("Write-Output 'CONTROL nonzero'; exit 23"))
[void](Run-Owned 'controller-positive' 10000 $pwsh @('-NoProfile', '-EncodedCommand', $positiveProgram) 0 'CONTROL positive')
[void](Run-Owned 'controller-nonzero' 10000 $pwsh @('-NoProfile', '-EncodedCommand', $nonzeroProgram) 23 'CONTROL nonzero')
$childProgram = 'Start-Sleep -Seconds 30'
$encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($childProgram))
$parentProgram = '$child = Start-Process -PassThru -FilePath ''' + $pwsh.Replace("'", "''") +
    ''' -ArgumentList @(''-NoProfile'', ''-EncodedCommand'', ''' + $encoded +
    '''); Write-Output (''CONTROL descendant='' + $child.Id); Start-Sleep -Seconds 30'
$control = Run-Owned 'controller-timeout' 5000 $pwsh @('-NoProfile', '-Command', $parentProgram) 124 'CONTROL descendant=[0-9]+'
if ($control -notmatch 'terminated=1 active_before_cleanup=[2-9]') { throw 'Actual descendant cleanup was not observed' }
[void](Run-Owned 'revocable-file' 45000 (Join-Path $Dir 'revocable_file_test.exe') @() 0 'revocable Windows contracts: [1-9][0-9]* checks, 0 failures')
[void](Run-Owned 'binder-revocable' 120000 (Join-Path $Dir 'binder_probe.exe') @() 0 'binder probe: all passed')
$sgScript = (Resolve-Path -LiteralPath 'tests/binder/run-host.ps1').Path
[void](Run-Owned 'binder-transport' 480000 $pwsh @('-NoProfile', '-File', $sgScript, '-Dir', $Dir) 0 'SG parent: [1-9][0-9]* checks, 0 failed')
Write-Host "REVOCABLE_COMPONENT_SUCCESS source=$head windows=$([Environment]::OSVersion.VersionString)"
