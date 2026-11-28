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
$work = Join-Path $env:TEMP ('gesher-signed-storage-migration-' + [guid]::NewGuid().ToString('N'))
$runtime = Join-Path $work 'App'
$fixture = Join-Path $work 'storage-migration.apk'
New-Item -ItemType Directory -Path $work | Out-Null
Copy-Item -LiteralPath $source -Destination $runtime -Recurse
Copy-Item -LiteralPath $apkSource -Destination $fixture
New-Item -ItemType File -Path (Join-Path $work 'portable.marker') | Out-Null
$package = 'org.gesher.contracts.storagemigration'
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

function Wait-Migration([string] $Step) {
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    $pattern = "StorageMigration: PASS step=$Step pid=(\d+) fixture-id=([a-f0-9-]+)"
    do {
        if (Test-Path -LiteralPath $log) {
            $events = @(Get-Content -LiteralPath $log)
            if ($events -match 'StorageMigration: FAIL ') {
                Get-Content -LiteralPath $log -Tail 30 | Write-Host
                throw "Migration failed in $Step"
            }
            $matching = @($events -match $pattern)
            if ($matching.Count -gt 1) { throw "Duplicate migration result for $Step" }
            if ($matching.Count -eq 1 -and $matching[0] -match $pattern) {
                Write-Host $matching[0]
                return @{ ProcessId = $Matches[1]; Token = $Matches[2] }
            }
        }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Missing migration result for $Step"
}

try {
    Invoke-Launcher ('install "' + $fixture + '"') 'install'
    $seen = @()
    $token = $null
    foreach ($step in 'ce-to-de','de-to-ce','verified') {
        Invoke-Launcher "launch $package --headless --keep-task --detach" $step
        $result = Wait-Migration $step
        if ($seen -contains $result.ProcessId -or ($token -and $token -ne $result.Token)) {
            throw "A process was reused or storage changed before $step"
        }
        $seen += $result.ProcessId
        $token = $result.Token
        Invoke-Launcher 'shutdown' "$step-shutdown" 45
    }
    'Windows signed storage migration verified across three processes'
} catch {
    if (Test-Path -LiteralPath $serverLog) { Get-Content -LiteralPath $serverLog -Tail 60 | Write-Host }
    throw
} finally {
    try {
        if (Test-Path -LiteralPath (Join-Path $profile 'data\system\gesher-device')) {
            Invoke-Launcher 'shutdown' 'cleanup-shutdown' 45
        }
    } finally {
        foreach ($name in $previous.Keys) { [Environment]::SetEnvironmentVariable($name, $previous[$name]) }
        Pop-Location
    }
}
