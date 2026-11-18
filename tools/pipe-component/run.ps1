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
$runner = Join-Path $Dir 'pipe_job_runner.exe'
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
# Native component fixtures use ordinary relative POSIX paths. Give their
# complete, job-owned process trees one isolated writable directory; bare
# Windows runners do not necessarily have the product's drive-root /tmp.
$fixtureDir = Join-Path ([IO.Path]::GetTempPath()) ('gesher-pipe-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($fixtureDir)
$savedTemp = $env:TEMP
$savedTmp = $env:TMP
$env:TEMP = $fixtureDir
$env:TMP = $fixtureDir
Push-Location -LiteralPath $fixtureDir
try {
    [void](Run-Owned 'stdio-ofd' 60000 (Join-Path $Dir 'stdio_ofd_test.exe') @() 0 'stdio OFD contracts: [1-9][0-9]* checks, 0 failures')
    [void](Run-Owned 'pipe-kernel' 45000 (Join-Path $Dir 'pipe_kernel_test.exe') @() 0 'pipe kernel contracts: [1-9][0-9]* checks, 0 failures')
    [void](Run-Owned 'pipe-process' 45000 (Join-Path $Dir 'pipe_native_process_test.exe') @() 0 'pipe native process contracts: [1-9][0-9]* checks, 0 failures')
    [void](Run-Owned 'file-flags' 45000 (Join-Path $Dir 'file_flags_ofd_test.exe') @() 0 'file_flags_ofd_test: [1-9][0-9]* assertions, 0 failures')
} finally {
    Pop-Location
    $env:TEMP = $savedTemp
    $env:TMP = $savedTmp
    Remove-Item -LiteralPath $fixtureDir -Recurse -Force
}
Write-Host "PIPE_COMPONENT_SUCCESS source=$head windows=$([Environment]::OSVersion.VersionString)"
