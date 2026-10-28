# CI-only independent-process SG modes. This runs ordinary actors only;
# the separately preserved public transaction-prefix transport-loss gate is open.
param(
    [Parameter(Mandatory)][string]$Dir,
    [Parameter(Mandatory)][string]$Fixtures,
    [Parameter(Mandatory)][string]$RuntimeRoot,
    [Parameter(Mandatory)][string]$Source,
    [ValidateSet('host','elf','art','single','device')][string[]]$Modes = @('host','elf','art','single','device')
)
$ErrorActionPreference = 'Stop'
$Dir = (Resolve-Path -LiteralPath $Dir).Path
$Fixtures = (Resolve-Path -LiteralPath $Fixtures).Path
$RuntimeRoot = (Resolve-Path -LiteralPath $RuntimeRoot).Path
if ((Get-Content "$Dir/binder-sg-runtime-source.txt" -Raw).Trim() -cne $Source) {
    throw 'SG current host/broker runtime source identity mismatch'
}
$manifest = Get-Content "$Fixtures/source.json" -Raw | ConvertFrom-Json
if ($manifest.source -cne $Source -or $manifest.run -cne $env:GITHUB_RUN_ID -or
    $manifest.attempt -cne $env:GITHUB_RUN_ATTEMPT -or @($manifest.files.PSObject.Properties).Count -ne 11) {
    throw 'SG actual JNI/ELF/dex/APK producer identity mismatch'
}
foreach ($entry in $manifest.files.PSObject.Properties) {
    $path = [IO.Path]::GetFullPath((Join-Path $Fixtures $entry.Name))
    if (-not $path.StartsWith($Fixtures + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
        (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $entry.Value) {
        throw "SG consumed fixture hash mismatch: $($entry.Name)"
    }
    Write-Host "ACTUAL_SG_FIXTURE member=$($entry.Name) sha256=$($entry.Value) source=$Source run=$($manifest.run)"
}
function NoDrive([string]$Path) { ($Path -replace '\\', '/') -replace '^[A-Za-z]:', '' }
$runRoot = Join-Path $env:RUNNER_TEMP ('gesher-sg-modes-' + [guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($runRoot)
$bcp = (('core-oj','core-libart','core-icu4j','framework','gesher-app') |
    ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$vm = @("-Xbootclasspath:$bcp", "-Ximage:$(NoDrive "$RuntimeRoot/system")/framework/boot.art", '-Xint', '-Xusejit:false')
$baseEnvironment = @{
    ANDROID_ROOT = NoDrive "$RuntimeRoot/system"
    ANDROID_ART_ROOT = NoDrive "$RuntimeRoot/art"
    ANDROID_I18N_ROOT = NoDrive "$RuntimeRoot/i18n"
    ANDROID_TZDATA_ROOT = NoDrive "$RuntimeRoot/tzdata"
    GESHER_REQUIRE_BOOT_IMAGE = '1'
    GESHER_TRACE_FAULTS = '1'
    GESHER_SHORTCUTS = '0'
    GESHER_BINDER_TRACE = '1'
}
$checks = 0
$failures = 0
function Assert-Mode([bool]$Condition, [string]$Label) {
    if (-not $Condition) { throw "SG mode assertion: $Label" }
    $script:checks++
    Write-Host "PASS SG mode: $Label"
}
function Start-Mode([string]$Exe, [string[]]$Arguments, [hashtable]$Environment, [string]$Log) {
    $info = [Diagnostics.ProcessStartInfo]::new($Exe)
    foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    # Each child gets its own environment; the CI parent never selects a driver.
    [void]$info.Environment.Remove('GESHER_BINDER')
    [void]$info.Environment.Remove('GESHER_BINDER_UID')
    foreach ($key in $Environment.Keys) { $info.Environment[$key] = $Environment[$key] }
    $child = [pscustomobject]@{
        Process = [Diagnostics.Process]::new(); Out = $null; Err = $null
        Log = $Log; Pid = 0; Text = ''; Exit = -1; Stopped = $false; Validated = $false
    }
    $child.Process.StartInfo = $info
    try {
        if (-not $child.Process.Start()) { throw "Cannot start SG child $Exe" }
        $child.Pid = $child.Process.Id
        $child.Out = $child.Process.StandardOutput.ReadToEndAsync()
        $child.Err = $child.Process.StandardError.ReadToEndAsync()
        return $child
    } catch {
        $setupFailure = $_
        try {
            if ($child.Pid -gt 0 -and -not $child.Process.HasExited) {
                $child.Process.Kill($true)
                if (-not $child.Process.WaitForExit(5000)) { throw 'SG setup child did not terminate' }
            }
        } finally { $child.Process.Dispose() }
        throw $setupFailure
    }
}
function Stop-Mode($Child, [int]$Milliseconds = 45000, [switch]$Terminate) {
    if ($Child.Stopped) { return }
    try {
        if ($Terminate -and -not $Child.Process.HasExited) {
            try { $Child.Process.Kill($true) }
            catch { if (-not $Child.Process.HasExited) { throw } }
        }
        $timedOut = -not $Child.Process.WaitForExit($Milliseconds)
        if ($timedOut) {
            try { $Child.Process.Kill($true) }
            catch { if (-not $Child.Process.HasExited) { throw } }
            if (-not $Child.Process.WaitForExit(5000)) { throw 'SG child did not terminate after finite kill deadline' }
        }
        if (-not [Threading.Tasks.Task]::WaitAll([Threading.Tasks.Task[]]@($Child.Out, $Child.Err), 5000)) {
            throw 'SG child retained redirected pipes beyond finite drain deadline'
        }
        $Child.Text = $Child.Out.Result + $Child.Err.Result
        $Child.Exit = if ($timedOut) { 124 } else { $Child.Process.ExitCode }
        [IO.File]::WriteAllText($Child.Log, $Child.Text)
        Write-Host $Child.Text
        Write-Host "ACTUAL_SG_CHILD pid=$($Child.Pid) exit=$($Child.Exit) freshExit=True log=$($Child.Log)"
    } finally {
        $Child.Stopped = $true
        $Child.Process.Dispose()
    }
}
function Wait-ModeFile([string]$Path, $Child) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while ($timer.ElapsedMilliseconds -lt 180000) {
        if ([IO.File]::Exists($Path)) { return }
        if ($Child.Process.HasExited) { throw "SG process exited before marker $Path" }
        Start-Sleep -Milliseconds 20
    }
    throw "SG process did not publish marker within finite readiness deadline: $Path"
}
function Start-Actor([string]$Mode, [string]$Abi, [string]$Role, [string]$Scenario,
                     [string]$Prefix, [string]$Device, [hashtable]$Environment) {
    $arguments = @($Role, $Scenario, $Prefix)
    $exe = "$Dir/binder_sg_actor.exe"
    switch ($Mode) {
        'host' { $Environment = $Environment.Clone(); $Environment.GESHER_BINDER = $Device }
        'elf' {
            $Environment = $Environment.Clone(); $Environment.GESHER_BINDER = $Device
            $exe = "$Dir/gesher-exec.exe"
            $arguments = @('--elf', "$Fixtures/$Abi/binder-sg-actor", 'binder-sg-actor', $Role, $Scenario, (NoDrive $Prefix))
        }
        'art' {
            $exe = "$Dir/dalvikvm.exe"
            $arguments = $vm + @('-cp', (NoDrive "$Fixtures/binder-sg.dex"), 'gesher.BinderSgActor',
                (NoDrive "$Fixtures/$Abi/libbindersg.so"), $Role, $Scenario, (NoDrive $Prefix), $Device)
        }
        'single' {
            $exe = "$Dir/dalvikvm.exe"
            $arguments = $vm + @('com.gesher.app.Main', (NoDrive "$Fixtures/binder-sg-$Role-$Abi.apk"))
        }
        'device' {
            $exe = "$Dir/dalvikvm.exe"
            $arguments = $vm + @('com.gesher.app.Launcher', 'launch', (NoDrive "$Fixtures/binder-sg-$Role-$Abi.apk"), '--headless')
        }
    }
    Start-Mode $exe $arguments $Environment "$Prefix.$Role.log"
}
function Assert-ActorCompletion($Child, [string]$Mode, [string]$Abi, [string]$Scenario, [string]$Role) {
    if ($Child.Validated) { return }
    Assert-Mode ($Child.Stopped -and $Child.Exit -eq 0 -and
        $Child.Text -match "SG ${Role}: [1-9][0-9]* checks, 0 failed" -and $Child.Text -notmatch '(?m)^FAIL ') `
        "$Mode/$Abi/$Scenario $Role actual assertions and fresh process exit0"
    if ($Mode -eq 'art') {
        Assert-Mode ($Child.Text.Contains("SG ART result: role=$Role; scenario=$Scenario; status=0; endpoint restored=true")) `
            "$Mode/$Abi/$Scenario $Role public endpoint restored"
    }
    if ($Mode -in 'single','device') {
        Assert-Mode ($Child.Text.Contains("BinderSgActivity onResume: all passed; role=$Role; scenario=$Scenario; mode=$Mode; endpoint restored=true; existing framework pings=")) `
            "$Mode/$Abi/$Scenario $Role existing framework identity/pings and endpoint restoration"
        $ids = [regex]::Matches($Child.Text, "SG APK identity: pid=([0-9]+); role=$Role; mode=$Mode;")
        Assert-Mode ($ids.Count -eq 1) "$Mode/$Abi/$Scenario $Role one actual APK identity"
        $actorId = [int]$ids[0].Groups[1].Value
    } else { $actorId = $Child.Pid }
    if ($Mode -eq 'device') {
        # This marker is published from the accepted generation/PID-bound
        # native process result, after real app death. Launcher0 alone fails.
        $package = [regex]::Escape("com.gesher.bindersg.$Role")
        $ended = [regex]::Matches($Child.Text, "(?m)^gesher-app: the process of $package ended \(exit code (-?[0-9]+)\)\r?$")
        Assert-Mode ($ended.Count -eq 1 -and $ended[0].Groups[1].Value -ceq '0') `
            "$Mode/$Abi/$Scenario $Role actual app normal exit-code-zero result"
        $started = [regex]::Matches($Child.Text, "app process for ${package}: pid ([0-9]+)")
        Assert-Mode ($started.Count -eq 1 -and [int]$started[0].Groups[1].Value -eq $actorId) `
            "$Mode/$Abi/$Scenario $Role app-process identity matches native actor PID"
    }
    $Child | Add-Member -NotePropertyName ActorPid -NotePropertyValue $actorId
    $stillRunning = $false
    try {
        $observed = [Diagnostics.Process]::GetProcessById($actorId)
        try { $stillRunning = -not $observed.HasExited }
        finally { $observed.Dispose() }
    } catch [ArgumentException] { }
    Assert-Mode (-not $stillRunning) "$Mode/$Abi/$Scenario $Role actual actor PID exited"
    $Child.Validated = $true
}
function Observe-OutputPage([string]$Prefix, $Server, [string]$Mode, [string]$Abi, [string]$Scenario) {
    if ($env:GITHUB_ACTIONS -cne 'true') { throw 'Native/C# observation is CI-only' }
    if (-not ('BinderOutputPageObserver' -as [type])) {
        Write-Host "ACTUAL_BINDER_OUTPUT_OBSERVER source=$Source sha256=$((Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'OutputPageObserver.cs') -Algorithm SHA256).Hash)"
        Add-Type -Path (Join-Path $PSScriptRoot 'OutputPageObserver.cs')
    }
    Wait-ModeFile "$Prefix.output-ready" $Server
    $fields = [IO.File]::ReadAllText("$Prefix.output-page").Trim().Split(' ')
    if ($fields.Count -ne 4) { throw 'Malformed actual page description' }
    $actor = [uint32]::Parse($fields[0]); $address = [uint64]::Parse($fields[1])
    $size = [int]::Parse($fields[2]); $protection = [uint32]::Parse($fields[3])
    $expected = if ($Scenario -eq 'readonly') { 2 } else { 4 }
    if ($protection -ne $expected) { throw 'Wrong actual page scenario' }
    $Server.OutputActorPid = $actor
    $observer = [BinderOutputPageObserver]::new($actor)
    try {
        $before = $observer.Inspect($address, $size, $protection, $true)
        Write-Host "ACTUAL_BINDER_OUTPUT_PAGE stage=before mode=$Mode abi=$Abi scenario=$Scenario pid=$actor address=$address bytes=$size protection=$protection sha256=$before"
        [IO.File]::WriteAllText("$Prefix.output-inspected", 'observed')
        Wait-ModeFile "$Prefix.output-complete" $Server
        $after = $observer.Inspect($address, $size, $protection, ($Scenario -eq 'readonly'))
        Assert-Mode $(if ($Scenario -eq 'readonly') { $after -ceq $before } else { $after -cne $before }) `
            "$Mode/$Abi/$Scenario actual whole-page bytes and unchanged protection observed independently"
        Write-Host "ACTUAL_BINDER_OUTPUT_PAGE stage=after mode=$Mode abi=$Abi scenario=$Scenario pid=$actor address=$address bytes=$size protection=$protection sha256=$after"
        [IO.File]::WriteAllText("$Prefix.output-observed", 'observed')
    } finally { $observer.Dispose() }
}

try {
    foreach ($mode in $Modes) {
        $abis = if ($mode -eq 'host') { @('host') } else { @('x86_64','arm64-v8a') }
        foreach ($abi in $abis) {
            foreach ($scenario in 'normal','malformed','oneway','smallmap','badread','prefixfault','adoptfail','readwrite','readonly') {
                $caseRoot = Join-Path $runRoot "$mode-$abi-$scenario"
                [void][IO.Directory]::CreateDirectory($caseRoot)
                [void][IO.Directory]::CreateDirectory("$caseRoot/data")
                $prefix = Join-Path $caseRoot 'actor'
                $device = 'sg88-' + [guid]::NewGuid().ToString('N')
                $environment = $baseEnvironment.Clone()
                $environment.ANDROID_DATA = NoDrive "$caseRoot/data"
                $environment.GESHER_LOGS = NoDrive "$caseRoot/device-logs"
                $environment.GESHER_SG_PREFIX = NoDrive $prefix
                $environment.GESHER_SG_DEVICE = $device
                $environment.GESHER_SG_SCENARIO = $scenario
                $environment.GESHER_SG_MODE = $mode
                $environment.LD_LIBRARY_PATH = NoDrive "$Fixtures/$abi"
                $broker = $null; $server = $null; $client = $null
                $children = [Collections.Generic.List[object]]::new()
                try {
                    $broker = Start-Mode "$Dir/binderd.exe" @($device) $environment "$prefix.broker.log"
                    $children.Add($broker)
                    $server = Start-Actor $mode $abi 'server' $scenario $prefix $device $environment
                    $children.Add($server)
                    Wait-ModeFile "$prefix.ready" $server
                    $client = Start-Actor $mode $abi 'client' $scenario $prefix $device $environment
                    $children.Add($client)
                    Wait-ModeFile "$prefix.queued" $client
                    if ($scenario -eq 'oneway') {
                        Stop-Mode $client
                        Assert-ActorCompletion $client $mode $abi $scenario 'client'
                        Assert-Mode $client.Validated "$mode/$abi/$scenario actual sender normal exit0 verified before receiver delivery"
                    }
                    [IO.File]::WriteAllText("$prefix.release", 'deliver')
                    if ($scenario -in 'readwrite','readonly') {
                        Observe-OutputPage $prefix $server $mode $abi $scenario
                    }
                    Stop-Mode $client
                    Stop-Mode $server
                    Assert-ActorCompletion $server $mode $abi $scenario 'server'
                    if ($scenario -in 'readwrite','readonly') {
                        Assert-Mode ($server.OutputActorPid -eq $server.ActorPid) "$mode/$abi/$scenario observer actual PID matches completed actor PID"
                    }
                    Assert-ActorCompletion $client $mode $abi $scenario 'client'
                    Assert-Mode ($server.ActorPid -gt 0 -and $client.ActorPid -gt 0 -and
                        $server.ActorPid -ne $client.ActorPid -and $server.ActorPid -ne $broker.Pid -and $client.ActorPid -ne $broker.Pid) `
                        "$mode/$abi/$scenario actual broker/server/client distinct PIDs"
                } catch {
                    $script:failures++
                    Write-Host "FAIL SG mode $mode/$abi/$scenario : $_"
                } finally {
                    if ($mode -eq 'device') {
                        try {
                            $shutdown = Start-Mode "$Dir/dalvikvm.exe" ($vm + @('com.gesher.app.Launcher','shutdown')) $environment "$prefix.shutdown.log"
                            $children.Add($shutdown)
                            Stop-Mode $shutdown 120000
                            Assert-Mode ($shutdown.Exit -eq 0) "$mode/$abi/$scenario actual device shutdown exit0"
                        } catch { $script:failures++; Write-Host "FAIL SG device shutdown: $_" }
                    }
                    foreach ($child in $children) {
                        if (-not $child.Stopped) {
                            try { Stop-Mode $child 5000 -Terminate }
                            catch { $script:failures++; Write-Host "FAIL SG owned-child cleanup: $_" }
                        }
                    }
                    if ($null -ne $broker -and $broker.Text) {
                        try {
                            # OpenBinder eagerly completes HelloProc before
                            # reserving/publishing the FD. ServeProc traces
                            # before replying; even unused successful SG FDs
                            # are counted here. Only thread ioctl pipes are lazy.
                            $opens = [regex]::Matches($broker.Text, 'proc ([0-9]+): pid ([0-9]+) opened the device')
                            Assert-Mode ($opens.Count -eq 2) "$mode/$abi/$scenario actual SG broker exactly two successful opens"
                            foreach ($actor in @($server,$client)) {
                                $matching = @($opens | Where-Object { [int]$_.Groups[2].Value -eq $actor.ActorPid })
                                Assert-Mode ($matching.Count -eq 1) "$mode/$abi/$scenario actual SG open belongs to actor PID=$($actor.ActorPid)"
                            }
                            Write-Host "ACTUAL_SG_OPEN_AUDIT mode=$mode abi=$abi scenario=$scenario broker=$($broker.Pid) server=$($server.ActorPid) client=$($client.ActorPid) opens=2 unexpected=0 scope=observed-run-only"
                        } catch { $script:failures++; Write-Host "FAIL SG actual broker open audit: $_" }
                    } else { $script:failures++; Write-Host 'FAIL SG actual broker trace is missing' }
                }
            }
        }
    }
} finally {
    Write-Host "SG mode logs retained: $runRoot"
}
Write-Host "SG modes: $checks checks, $failures failed; broader concurrent-new-open isolation and public-prefix transport-loss proof remain open"
if ($failures) { exit 1 }
