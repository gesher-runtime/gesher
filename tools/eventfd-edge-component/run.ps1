param([Parameter(Mandatory)][string]$Dir)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$Dir = (Resolve-Path -LiteralPath $Dir).Path
$receipt = Get-Content -LiteralPath (Join-Path $Dir 'receipt.json') -Raw | ConvertFrom-Json
$head = (git rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $receipt.schema -ne 1 -or $receipt.dirty -or
    $receipt.head -cne $head -or $head -cne $env:GITHUB_SHA -or
    $receipt.source_commit -cne 'ea4a46073dc082475ab2c6d431af7448fcb09a11' -or
    $receipt.source_tree -cne '61648f334dc2b344efbbde75bc679d206fa04173' -or
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
$runner = Join-Path $Dir 'eventfd_job_runner.exe'
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
# A zero exit from the root cannot hide a descendant that remains alive after
# the bounded natural-retirement interval. The owner must reject and reap it.
$orphanProgram = '$child = Start-Process -PassThru -FilePath ''' + $pwsh.Replace("'", "''") +
    ''' -ArgumentList @(''-NoProfile'', ''-EncodedCommand'', ''' + $encoded +
    '''); Write-Output (''CONTROL live-descendant='' + $child.Id); exit 0'
$orphanEncoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($orphanProgram))
$orphanControl = Run-Owned 'controller-live-descendant' 10000 $pwsh @('-NoProfile', '-EncodedCommand', $orphanEncoded) 125 'CONTROL live-descendant=[0-9]+'
if ($orphanControl -notmatch 'root_exit=0 result=125 empty=1 terminated=1 active_before_cleanup=[1-9]') {
    throw 'Successful-root live descendant was not rejected and cleaned'
}
# Execute the unchanged public API and complete-production wait tests under
# the already qualified Win32 process-tree owner. No host adapters are linked.
[void](Run-Owned 'eventfd-edge' 45000 (Join-Path $Dir 'epoll_probe.exe') @('--eventfd-edge') 0 'eventfd edge Windows: all passed')
$edgeText = Get-Content -LiteralPath (Join-Path $logs 'eventfd-edge.log') -Raw
if ($edgeText -match '(?m)^FAIL' -or
    $edgeText -notmatch 'eventfd edge contracts: complete' -or
    $edgeText -notmatch 'eventfd edge wait contracts: complete') {
    throw 'Actual production eventfd probe omitted or failed a required contract group'
}
Write-Host "EVENTFD_EDGE_COMPONENT_SUCCESS source=$head reviewed=$($receipt.source_commit) windows=$([Environment]::OSVersion.VersionString)"
