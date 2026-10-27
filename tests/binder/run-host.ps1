param([Parameter(Mandatory)][string]$Dir)
$ErrorActionPreference = 'Stop'
$actor = (Resolve-Path -LiteralPath (Join-Path $Dir 'binder_sg_actor.exe')).Path
$wireActor = (Resolve-Path -LiteralPath (Join-Path $Dir 'binder_sg_wire_peer.exe')).Path
$relayExe = (Resolve-Path -LiteralPath (Join-Path $Dir 'binder_sg_finish_relay.exe')).Path
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
function Start-Sg([string]$Exe, [string[]]$Arguments, [string]$Log, [string]$Device = '') {
    # Fixture prefixes have no embedded quote. Use ProcessStartInfo with a
    # hidden window and real redirected streams; no shell interpretation.
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $Exe
    $info.Arguments = ($Arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    if ($Device) { $info.Environment['GESHER_BINDER'] = $Device }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $info
    try {
        if (-not $process.Start()) { throw "Cannot start $Exe" }
        return [pscustomobject]@{ Process = $process; Out = $process.StandardOutput.ReadToEndAsync(); Err = $process.StandardError.ReadToEndAsync(); Log = $Log }
    } catch {
        $setupFailure = $_
        # Setup can fail after Start succeeds. Reap/dispose that actual owned
        # child even when there is no completed Child record for the caller.
        try {
            if (-not $process.HasExited) { $process.Kill() }
            if (-not $process.WaitForExit(5000)) { Write-Host 'FAIL SG parent: setup-failure child did not exit after kill' }
        } catch { Write-Host "FAIL SG parent: setup cleanup: $_" }
        finally { $process.Dispose() }
        throw $setupFailure
    }
}
function Stop-Sg($Child, [int]$Deadline = 45000) {
    if ($null -eq $Child) { return -1 }
    $process = $Child.Process
    $timedOut = -not $process.WaitForExit($Deadline)
    if ($timedOut) {
        try { $process.Kill() } catch { }
        if (-not $process.WaitForExit(5000)) { throw 'SG child did not terminate within five seconds after kill' }
    }
    if (-not [Threading.Tasks.Task]::WaitAll([Threading.Tasks.Task[]]@($Child.Out, $Child.Err), 5000)) {
        throw 'SG child retained redirected pipes after finite drain deadline'
    }
    $text = $Child.Out.Result + $Child.Err.Result
    [IO.File]::WriteAllText($Child.Log, $text)
    Write-Host $text
    $code = if ($timedOut) { 124 } else { $process.ExitCode }
    $process.Dispose()
    return $code
}
function Wait-SgFile([string]$Path, $Child) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while ($timer.ElapsedMilliseconds -lt 30000) {
        if ([IO.File]::Exists($Path)) { return $true }
        if ($null -ne $Child -and $Child.Process.HasExited) { return $false }
        Start-Sleep -Milliseconds 20
    }
    return $false
}
try {
    foreach ($case in @('normal', 'malformed', 'oneway', 'smallmap', 'badread', 'prefixfault', 'adoptfail', 'public-finishloss',
        'wire-beforeclaim', 'wire-claimloss', 'wire-installabort', 'wire-finishloss', 'wire-queue',
        'wire-death-beforeclaim', 'wire-death-afterclaim', 'wire-death-afterinstall', 'wire-death-afterfinish')) {
        $env:GESHER_BINDER = 'sg88-' + [guid]::NewGuid().ToString('N')
        $prefix = Join-Path $root $case
        $broker = $null; $server = $null; $client = $null; $relay = $null
        try {
            $broker = Start-Sg $brokerExe @($env:GESHER_BINDER) ($prefix + '.broker.log')
            $serverDevice = $env:GESHER_BINDER
            if ($case -eq 'public-finishloss') {
                $serverDevice = $env:GESHER_BINDER + '-front'
                $relay = Start-Sg $relayExe @($serverDevice, $env:GESHER_BINDER, $prefix) ($prefix + '.relay.log')
                if (-not (Wait-SgFile ($prefix + '.proxy-ready') $relay)) { throw 'Transparent finish relay did not become ready' }
            }
            # Probe the real driver endpoint without opening a competing
            # context manager. Actors retry their actual open below.
            $serverExe = if ($case.StartsWith('wire-')) { $wireActor } else { $actor }
            $server = Start-Sg $serverExe @('server', $case, $prefix) ($prefix + '.server.log') $serverDevice
            if (-not (Wait-SgFile ($prefix + '.ready') $server)) { throw "Receiver not ready: $case" }
            $client = Start-Sg $actor @('client', $case, $prefix) ($prefix + '.client.log')
            Assert-Sg ($client.Process.Id -ne $server.Process.Id -and $server.Process.Id -ne $broker.Process.Id -and
                $client.Process.Id -ne $broker.Process.Id) "$case broker/sender/receiver have three actual distinct PIDs"
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
            if ($case -eq 'public-finishloss') {
                Assert-Sg ([IO.File]::Exists($prefix + '.ack-dropped')) 'public finish loss verified real broker success ACK before withholding it'
            }
        } finally {
            $cleanupErrors = @()
            foreach ($child in @($client, $server, $relay, $broker)) {
                if ($null -ne $child) {
                    try { [void](Stop-Sg $child 1000) }
                    catch { $cleanupErrors += $_.Exception.Message; $child.Process.Dispose() }
                }
            }
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
