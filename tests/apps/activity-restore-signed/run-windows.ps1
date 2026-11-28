# SPDX-License-Identifier: Apache-2.0
param(
    [Parameter(Mandatory)] [string] $PortableApp,
    [Parameter(Mandatory)] [string] $Apk
)
$ErrorActionPreference = 'Stop'
$source = (Resolve-Path -LiteralPath $PortableApp).Path
$apkSource = (Resolve-Path -LiteralPath $Apk).Path
if (!(Test-Path -LiteralPath (Join-Path $source 'gesher-app.cmd'))) {
    throw 'PortableApp must name the prepared runtime App directory'
}
$work = Join-Path $env:TEMP ('gesher-signed-restore-' + [guid]::NewGuid().ToString('N').Substring(0,8))
$runtime = Join-Path $work 'App'
$fixture = Join-Path $work 'restore.apk'
New-Item -ItemType Directory -Path $work | Out-Null
Copy-Item -LiteralPath $source -Destination $runtime -Recurse
Copy-Item -LiteralPath $apkSource -Destination $fixture
New-Item -ItemType File -Path (Join-Path $work 'portable.marker') | Out-Null
$package = 'com.gesher.restorefixture'
$launcher = Join-Path $runtime 'gesher-app.cmd'
$profile = Join-Path $work 'profile'
$logs = Join-Path $profile 'logs'
$log = Join-Path $logs "$package.log"
$serverLog = Join-Path $logs 'system_server.log'
$environment = @{
    GESHER_PROFILE = $profile
    GESHER_LOGS = $null
    GESHER_IDLE_TIMEOUT = '120'
    GESHER_LOCALES = 'en-US'
}
"Owned fixture work directory $work"
Get-FileHash -LiteralPath $fixture,(Join-Path $runtime 'gesher-app.jar'),(Join-Path $runtime 'dalvikvm.exe')
$previous = @{}
Push-Location $work
foreach ($name in $environment.Keys) {
    $previous[$name] = [Environment]::GetEnvironmentVariable($name)
    [Environment]::SetEnvironmentVariable($name, $environment[$name])
}

function Invoke-Launcher($command, $label, [int] $seconds=180) {
    $out = Join-Path $work "$label.out"
    $err = Join-Path $work "$label.err"
    $arguments = '/d /s /c ""' + $launcher + '" ' + $command + '"'
    $process = Start-Process $env:ComSpec -ArgumentList $arguments -PassThru -NoNewWindow -RedirectStandardOutput $out -RedirectStandardError $err
    $null = $process.Handle
    if (!$process.WaitForExit($seconds * 1000)) {
        & taskkill.exe /pid $process.Id /T /F | Out-Null
        Get-Content $out -Tail 30 | Write-Host
        Get-Content $err -Tail 30 | Write-Host
        if (Test-Path $serverLog) { Get-Content $serverLog -Tail 35 | Write-Host }
        throw "Timed out waiting for $label"
    }
    "$label exit=$($process.ExitCode)"
    if ($process.ExitCode -ne 0) {
        Get-Content $out -Tail 15
        Get-Content $err -Tail 15
        throw "$label failed"
    }
}

function Read-Events {
    if (Test-Path -LiteralPath $log) {
        return ((Get-Content -LiteralPath $log | Where-Object { $_ -match 'I F03Restore:' }) -join "`n")
    }
    return ''
}

try {
    Invoke-Launcher ('install "' + $fixture + '"') 'install'
    Invoke-Launcher "launch $package --headless --keep-task --detach" 'launch'
    $deadline = [DateTime]::UtcNow.AddSeconds(35)
    do {
        $events = Read-Events
        if ($events -match 'PARENT_SAVED' -and $events -match 'CHILD_READY') { break }
        Start-Sleep -Milliseconds 150
    } while ([DateTime]::UtcNow -lt $deadline)
    $events
    $old = [regex]::Match($events, 'PARENT_FIRST pid=(\d+) task=(\d+)')
    $child = [regex]::Match($events, 'CHILD_READY pid=(\d+) parent=(\d+) task=(\d+)')
    if (!$old.Success -or !$child.Success -or $events -notmatch 'PARENT_STOPPED' -or $events -notmatch 'PARENT_SAVED' -or
        $child.Groups[2].Value -ne $old.Groups[1].Value -or $child.Groups[3].Value -ne $old.Groups[2].Value) {
        throw 'No saved parent in the independent child task'
    }
    $parentPid = [int]$old.Groups[1].Value
    $childPid = [int]$child.Groups[1].Value
    if ($parentPid -eq $childPid) { throw 'The signed fixture did not create separate processes' }
    $ownedRuntime = Join-Path $runtime 'dalvikvm.exe'
    $parentProcess = [Diagnostics.Process]::GetProcessById($parentPid)
    $childProcess = [Diagnostics.Process]::GetProcessById($childPid)
    try {
        $null = $parentProcess.Handle
        $null = $childProcess.Handle
        if ($parentProcess.MainModule.FileName -ne $ownedRuntime -or $childProcess.MainModule.FileName -ne $ownedRuntime) {
            throw 'A returned PID does not belong to the disposable runtime copy'
        }
        $parentProcess.Kill()
        if (!$parentProcess.WaitForExit(5000) -or $childProcess.HasExited) { throw 'The retained child did not survive parent exit' }
        "PARENT_PROCESS_EXITED old=$parentPid childStillLive=$childPid"
    } finally {
        $parentProcess.Dispose()
        $childProcess.Dispose()
    }
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    $observedDeath = $false
    do {
        if ((Test-Path $serverLog) -and ((Get-Content -LiteralPath $serverLog -Raw) -match "process $parentPid .*died")) {
            $observedDeath = $true
            break
        }
        Start-Sleep -Milliseconds 150
    } while ([DateTime]::UtcNow -lt $deadline)
    if (!$observedDeath) { throw 'No system_server parent-death record' }
    $release = Join-Path $env:GESHER_PROFILE "data\data\$package\files\release-result"
    New-Item -ItemType File -Path $release | Out-Null
    $deadline = [DateTime]::UtcNow.AddSeconds(35)
    do {
        $events = Read-Events
        if ($events -match 'PASS restored task and view received the child result exactly once' -or $events -match 'FAIL ') { break }
        Start-Sleep -Milliseconds 150
    } while ([DateTime]::UtcNow -lt $deadline)
    Start-Sleep -Seconds 1
    $events = Read-Events
    $events
    if ($events -notmatch "PARENT_RESTORED pid=\d+ old=$parentPid task=$($old.Groups[2].Value) valid=true" -or
        $events -notmatch 'PARENT_VIEW valid=true' -or
        [regex]::Matches($events, 'PARENT_RESULT request=702 valid=true').Count -ne 1 -or
        [regex]::Matches($events, 'PASS restored task and view received the child result exactly once').Count -ne 1) {
        throw "The signed Windows fixture failed its targeted restoration/result checks"
    }
} catch {
    if (Test-Path $serverLog) { Get-Content -LiteralPath $serverLog -Tail 90 | Write-Host }
    throw
} finally {
    try {
        $deviceFile = Join-Path $env:GESHER_PROFILE 'data\system\gesher-device'
        if (Test-Path -LiteralPath $deviceFile) { Invoke-Launcher 'shutdown' 'shutdown' 45 }
    } finally {
        foreach ($name in $previous.Keys) { [Environment]::SetEnvironmentVariable($name, $previous[$name]) }
        Pop-Location
    }
}
