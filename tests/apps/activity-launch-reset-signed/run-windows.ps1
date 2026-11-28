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
$work = Join-Path $env:TEMP ('gesher-signed-launch-reset-' + [guid]::NewGuid().ToString('N'))
$runtime = Join-Path $work 'App'
$fixture = Join-Path $work 'launch-reset.apk'
New-Item -ItemType Directory -Path $work | Out-Null
Copy-Item -LiteralPath $source -Destination $runtime -Recurse
Copy-Item -LiteralPath $apkSource -Destination $fixture
New-Item -ItemType File -Path (Join-Path $work 'portable.marker') | Out-Null
$package = 'com.gesher.launchreset'
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

function Invoke-Launcher([string] $Command, [string] $Label, [int] $Seconds = 90) {
    $out = Join-Path $work "$Label.out"
    $err = Join-Path $work "$Label.err"
    $arguments = '/d /s /c ""' + $launcher + '" ' + $Command + '"'
    $process = Start-Process $env:ComSpec -ArgumentList $arguments -PassThru -NoNewWindow -RedirectStandardOutput $out -RedirectStandardError $err
    try {
        $null = $process.Handle
        if (!$process.WaitForExit($Seconds * 1000)) {
            & taskkill.exe /pid $process.Id /T /F | Out-Null
            throw "Timed out waiting for $Label"
        }
        if ($process.ExitCode -ne 0) {
            throw "$Label exited $($process.ExitCode)"
        }
    } catch {
        Get-Content -LiteralPath $out,$err -Tail 30 | Write-Host
        throw
    } finally {
        $process.Dispose()
    }
}

function Wait-ProbeEvents([string] $Pattern) {
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        if (Test-Path -LiteralPath $log) {
            $events = (Get-Content -LiteralPath $log | Where-Object { $_ -match 'I F01LauncherReset:' }) -join "`n"
            if ($events -match $Pattern) { return $events }
        }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Missing probe event $Pattern. Events: $events"
}

try {
    Invoke-Launcher ('install "' + $fixture + '"') 'install'
    $open = "launch $package --headless --keep-task --detach"
    Invoke-Launcher $open 'main-open'
    $events = Wait-ProbeEvents 'CHILD_RESUME'
    $initial = '(?s)ROOT_CREATE count=1 task=(?<task>\d+) flags=10200000.*CHILD_RESUME task=\k<task>\b'
    if ($events -notmatch $initial) { throw "Wrong initial flags or task: $events" }

    Invoke-Launcher $open 'main-reopen'
    $null = Wait-ProbeEvents 'ROOT_RESUME count=2'
    $events = Wait-ProbeEvents 'CHILD_DESTROY'
    if ([regex]::Matches($events, 'ROOT_CREATE ').Count -ne 1 -or
        $events -notmatch ($initial + '.*ROOT_RESUME count=2 task=\k<task>\b') -or
        $events -notmatch ($initial + '.*CHILD_DESTROY task=\k<task> finishing=true\b')) {
        throw "The launch did not reset the original task: $events"
    }

    Invoke-Launcher 'launch com.gesher.launchreset --activity com.gesher.launchreset.LaunchRoot$Requested --headless --keep-task --detach' 'explicit'
    $null = Wait-ProbeEvents 'REQUESTED action=android.intent.action.MAIN flags=10000000 data=null'
    Invoke-Launcher 'view com.gesher.launchreset gesher-task-probe://requested' 'view'
    $events = Wait-ProbeEvents 'REQUESTED action=android.intent.action.VIEW flags=10000000 data=gesher-task-probe://requested'
    $events
    'Windows launcher reset and explicit component and URI launches verified'
} catch {
    if (Test-Path -LiteralPath $serverLog) { Get-Content -LiteralPath $serverLog -Tail 60 | Write-Host }
    throw
} finally {
    try {
        if (Test-Path -LiteralPath (Join-Path $profile 'data\system\gesher-device')) {
            Invoke-Launcher 'shutdown' 'shutdown' 45
        }
    } finally {
        foreach ($name in $previous.Keys) { [Environment]::SetEnvironmentVariable($name, $previous[$name]) }
        Pop-Location
    }
}
