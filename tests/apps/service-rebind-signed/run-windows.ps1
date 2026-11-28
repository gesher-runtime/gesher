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
$work = Join-Path $env:TEMP ('gesher-signed-service-rebind-' + [guid]::NewGuid().ToString('N'))
$runtime = Join-Path $work 'App'
$fixture = Join-Path $work 'service-rebind.apk'
New-Item -ItemType Directory -Path $work | Out-Null
Copy-Item -LiteralPath $source -Destination $runtime -Recurse
Copy-Item -LiteralPath $apkSource -Destination $fixture
New-Item -ItemType File -Path (Join-Path $work 'portable.marker') | Out-Null
$package = 'gesher.contract.svcdeath'
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
            $events = (Get-Content -LiteralPath $log | Where-Object { $_ -match 'ServiceDeathProbe:' }) -join "`n"
            if ($events -match $Pattern) { return $events }
        }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Missing probe event $Pattern. Events: $events"
}

try {
    Invoke-Launcher ('install "' + $fixture + '"') 'install'
    Invoke-Launcher "launch $package --headless --keep-task --detach" 'launch'
    $events = Wait-ProbeEvents 'ServiceDeathProbe: (PASS|FAIL)'
    $events
    if ($events -notmatch 'ServiceDeathProbe: PASS connected=2 disconnected=1 unexpected=0' -or
        $events -match 'ServiceDeathProbe: (UNEXPECTED_|FAIL )') {
        throw 'Unexpected service recovery callbacks'
    }
    'Windows signed service process recovery verified'
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
