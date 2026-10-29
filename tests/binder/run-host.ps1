param([Parameter(Mandatory)][string]$Dir)
$ErrorActionPreference = 'Stop'
$actor = (Resolve-Path -LiteralPath (Join-Path $Dir 'binder_sg_actor.exe')).Path
$wireActor = (Resolve-Path -LiteralPath (Join-Path $Dir 'binder_sg_wire_peer.exe')).Path
$brokerExe = (Resolve-Path -LiteralPath (Join-Path $Dir 'binderd.exe')).Path
$root = Join-Path ([IO.Path]::GetTempPath()) ('gesher-sg-' + [guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($root)
$oldDevice = $env:GESHER_BINDER
$failed = 0
$checks = 0
function Assert-Sg([bool]$Condition, [string]$Label) {
    $script:checks++
    if (-not $Condition) { $script:failed++; Write-Host "FAIL SG parent: $Label" }
    else { Write-Host "PASS SG parent: $Label" }
}
 . "$PSScriptRoot/trusted-sg-client.ps1"
function Start-Sg([string]$Exe, [string[]]$Arguments, [string]$Log, [string]$Role) {
    $environment = @{ GESHER_BINDER = $env:GESHER_BINDER }
    if ($Role -eq 'broker') {
        $answer = Invoke-TrustedSgOwner $owner @{ op='begin'; broker=$Exe; endpoint=$env:GESHER_BINDER; environment=$environment }
    } else {
        $fixtureRole = if ($Role -eq 'server') { '@servicemanager' } else { '@system_server' }
        $answer = Invoke-TrustedSgOwner $owner @{ op='spawn'; args=@($Exe) + $Arguments; role=$fixtureRole; environment=$environment }
    }
    return [pscustomobject]@{ Id=[int]$answer.id; Pid=[int]$answer.pid; Log=$Log; Stopped=$false; Exit=$null }
}
function Get-SgStatus($Child) {
    return (Invoke-TrustedSgOwner $owner @{ op='status'; id=$Child.Id }).exit
}
function Stop-Sg($Child, [int]$Deadline = 45000) {
    if ($null -eq $Child) { return -1 }
    if ($Child.Stopped) { return $Child.Exit }
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $code = Get-SgStatus $Child
    while ($null -eq $code -and $timer.ElapsedMilliseconds -lt $Deadline) {
        Start-Sleep -Milliseconds 20
        $code = Get-SgStatus $Child
    }
    $timedOut = $null -eq $code
    if ($timedOut) {
        [void](Invoke-TrustedSgOwner $owner @{ op='terminate'; id=$Child.Id })
        $timer.Restart()
        do {
            Start-Sleep -Milliseconds 20
            $code = Get-SgStatus $Child
        } while ($null -eq $code -and $timer.ElapsedMilliseconds -lt 5000)
        if ($null -eq $code) { throw 'exact owned SG child did not terminate' }
    }
    $text = Read-TrustedSgOutput $owner $Child.Id
    [IO.File]::WriteAllText($Child.Log, $text)
    Write-Host $text
    $Child.Exit = if ($timedOut) { 124 } else { [long]$code }
    $Child.Stopped = $true
    return $Child.Exit
}
function Wait-SgFile([string]$Path, $Child) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while ($timer.ElapsedMilliseconds -lt 30000) {
        if ([IO.File]::Exists($Path)) { return $true }
        if ($null -ne $Child -and $null -ne (Get-SgStatus $Child)) { return $false }
        Start-Sleep -Milliseconds 20
    }
    return $false
}

try {
    foreach ($case in @('normal', 'malformed', 'oneway', 'smallmap', 'badread', 'prefixfault', 'adoptfail',
        'wire-beforeclaim', 'wire-claimloss', 'wire-installabort', 'wire-finishloss', 'wire-queue',
        'wire-death-beforeclaim', 'wire-death-afterclaim', 'wire-death-afterinstall', 'wire-death-afterfinish')) {
        $env:GESHER_BINDER = 'sg88-' + [guid]::NewGuid().ToString('N')
        $prefix = Join-Path $root $case
        $broker = $null; $server = $null; $client = $null
        $owner = Start-TrustedSgOwner
        try {
            $broker = Start-Sg $brokerExe @($env:GESHER_BINDER) ($prefix + '.broker.log') 'broker'
            # Probe the real driver endpoint without opening a competing
            # context manager. Actors retry their actual open below.
            $serverExe = if ($case.StartsWith('wire-')) { $wireActor } else { $actor }
            $server = Start-Sg $serverExe @('server', $case, $prefix) ($prefix + '.server.log') 'server'
            if (-not (Wait-SgFile ($prefix + '.ready') $server)) { throw "Receiver not ready: $case" }
            $client = Start-Sg $actor @('client', $case, $prefix) ($prefix + '.client.log') 'client'
            Assert-Sg ($client.Pid -ne $server.Pid -and $server.Pid -ne $broker.Pid -and
                $client.Pid -ne $broker.Pid) "$case broker/sender/receiver have three actual distinct PIDs"
            if (-not (Wait-SgFile ($prefix + '.queued') $client)) { throw "Sender did not queue: $case" }
            if ($case -in @('oneway', 'wire-queue')) {
                Assert-Sg ((Stop-Sg $client) -eq 0) "$case sender exits before receiver reads"
                $client = $null
            }
            [IO.File]::WriteAllText($prefix + '.release', 'deliver')
            if ($null -ne $client) {
                Assert-Sg ((Stop-Sg $client) -eq 0) "$case sender assertions and bounded completion"
                $client = $null
            }
            Assert-Sg ((Stop-Sg $server) -eq 0) "$case receiver assertions and bounded completion"
            $server = $null
            if ($case.StartsWith('wire-death-')) {
                Assert-Sg ([IO.File]::Exists($prefix + '.death')) "$case reached exact real process-death boundary"
            }
        } finally {
            $cleanupErrors = @()
            foreach ($child in @($client, $server, $broker)) {
                if ($null -ne $child) {
                    try { [void](Stop-Sg $child 1000) }
                    catch { $cleanupErrors += $_.Exception.Message }
                }
            }
            try { Close-TrustedSgOwner $owner } catch { $cleanupErrors += $_.Exception.Message }
            if ($cleanupErrors.Count) { throw ('SG cleanup failures after attempting all owned children: ' + ($cleanupErrors -join ' | ')) }
        }
    }
} finally {
    $env:GESHER_BINDER = $oldDevice
    # Preserve logs on every outcome. CI publishes this directory explicitly.
    Write-Host "SG fixture logs: $root"
}
Write-Host "SG parent: $checks checks, $failed failed"
if ($failed) { exit 1 }
