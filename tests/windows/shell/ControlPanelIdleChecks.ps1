param([string] $PanelPath = (Join-Path $PSScriptRoot '../../../tools/package/gesher-control-panel.ps1'))

$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'ControlPanelIdleCounterChecks.ps1') -FixturePath $PSCommandPath
Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase
$nativeCompile = [Diagnostics.Stopwatch]::StartNew()
. (Join-Path (Split-Path -Parent ([IO.Path]::GetFullPath($PanelPath))) 'gesher-panel-native.ps1')
$nativeCompile.Stop()
Write-Host "Panel native helper compile_ms=$($nativeCompile.ElapsedMilliseconds) (single existing startup compilation; not a cold-start benchmark)"

# Execute the production owner, event wiring, stamps, process observer and
# heartbeat against a real WPF dispatcher and real temporary files/processes.
# Only application-list parsing/rendering and unrelated heartbeat duties are
# declared boundaries; their existing action/update/request gates still run.
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile([IO.Path]::GetFullPath($PanelPath), [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw ($errors | Out-String) }
foreach ($name in 'Get-FileStamp','Get-StateStamp','Get-AppFilesStamp','Get-DeviceStatus',
        'Test-PanelMetadataVisible','Request-PanelMetadataRefresh','Update-PanelMetadataWatch',
        'Invoke-PanelMetadataVisibilityChanged','Invoke-PanelMetadataActivated','Initialize-PanelMetadata',
        'Close-PanelMetadata','Refresh-Dashboard','Invoke-LiveRefreshTick','Invoke-PanelHeartbeat') {
    $definition = @($ast.FindAll({ param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
    }, $false))
    if ($definition.Count -ne 1) { throw "Expected one production function: $name" }
    . ([scriptblock]::Create($definition[0].Extent.Text))
}
$script:ReadState = ${function:Get-StateStamp}
$script:ReadApps = ${function:Get-AppFilesStamp}
$script:ReadDevice = ${function:Get-DeviceStatus}
$script:UiThread = [Threading.Thread]::CurrentThread.ManagedThreadId
$script:Checks = 0
function Assert-Idle([bool] $okay, [string] $reason) {
    if (-not $okay) { throw $reason }
    $script:Checks++
}
function Assert-UiThread {
    if ([Threading.Thread]::CurrentThread.ManagedThreadId -ne $script:UiThread) { throw 'Metadata work crossed the WPF dispatcher thread' }
}
function Get-StateStamp { Assert-UiThread; $script:Scans.State++; & $script:ReadState }
function Get-AppFilesStamp { Assert-UiThread; $script:Scans.Apps++; & $script:ReadApps }
function Get-DeviceStatus { Assert-UiThread; $script:Scans.Process++; & $script:ReadDevice }
function Get-MicrogStatus { $false }
function Update-DeviceView([bool] $running) { Assert-UiThread; $script:ShownDevice = $running }
function Update-MicrogView([bool] $enabled) { Assert-UiThread }
function Update-StoreView($installed) { Assert-UiThread }
function Get-InstalledApps {
    Assert-UiThread
    $script:Scans.List++
    foreach ($package in [IO.File]::ReadAllLines($script:PackagesPath)) {
        if (-not $package) { continue }
        $shortcut = Join-Path $root "data/data/$package/gesher-shell/shortcut.properties"
        $label = if ([IO.File]::Exists($shortcut)) { [IO.File]::ReadAllText($shortcut) } else { $package }
        [pscustomobject]@{ Package = $package; Label = $label }
    }
}
function Update-AppsView($installed) {
    Assert-UiThread
    $script:Draws++
    $script:ShownPackages = @($installed | ForEach-Object { $_.Package })
    $script:ShownLabels = @($installed | ForEach-Object { $_.Label })
    $paths = @()
    foreach ($app in $installed) {
        $paths += Join-Path $root "data/data/$($app.Package)/gesher-shell/shortcut.properties"
        $paths += Join-Path $root "data/data/$($app.Package)/gesher-shell/icon.ico"
    }
    $script:ViewState.AppFiles = $paths
}
function Complete-PanelAction { Assert-UiThread; $script:Duties.Action++ }
function Invoke-PanelUpdateTick {
    Assert-UiThread
    $script:Duties.Update++
    if ($script:CancelPending) { $script:CancelPending = $false; $script:Duties.Cancel++ }
}
function Invoke-PanelRequest {
    Assert-UiThread
    $script:Duties.Activation++
    if ($script:ShowPending) {
        $script:ShowPending = $false
        $window.Show()
        $window.WindowState = [Windows.WindowState]::Normal
    }
}
function Set-PanelUpdateStatus { throw 'Unexpected update failure' }
function Update-PanelUpdateView { }
function Scan-Total { $script:Scans.State + $script:Scans.Apps + $script:Scans.Process + $script:Scans.List }

function Pump-Until([scriptblock] $Condition, [string] $Reason, [int] $TimeoutMs = 6000) {
    $script:PumpCondition = $Condition
    $script:PumpReason = $Reason
    $script:PumpFailure = $null
    $script:PumpDeadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    $script:PumpFrame = [Windows.Threading.DispatcherFrame]::new()
    $timer = [Windows.Threading.DispatcherTimer]::new()
    # Short ticks exercise scheduling without waiting for the normal 500ms UI
    # cadence; production debounce/recovery durations are unchanged below.
    $timer.Interval = [TimeSpan]::FromMilliseconds(20)
    $timer.Add_Tick({
        try {
            Invoke-PanelHeartbeat
            if (& $script:PumpCondition) { $script:PumpFrame.Continue = $false }
            elseif ([DateTime]::UtcNow -ge $script:PumpDeadline) { throw $script:PumpReason }
        } catch { $script:PumpFailure = $_; $script:PumpFrame.Continue = $false }
    })
    try { $timer.Start(); [Windows.Threading.Dispatcher]::PushFrame($script:PumpFrame) }
    finally { $timer.Stop() }
    if ($script:PumpFailure) { throw $script:PumpFailure }
}
function Pump-For([int] $Milliseconds) {
    $script:RunUntil = [DateTime]::UtcNow.AddMilliseconds($Milliseconds)
    Pump-Until { [DateTime]::UtcNow -ge $script:RunUntil } 'Dispatcher duration exceeded its bound' ($Milliseconds + 3000)
}
function Wait-ManagedNotification([long] $Before) {
    # Intentionally do not pump WPF: a pool-thread event may queue one managed
    # notification but must not invoke any PowerShell callback on that thread.
    $deadline = [DateTime]::UtcNow.AddSeconds(3)
    while ($script:MetadataMonitor.Generation -le $Before -and [DateTime]::UtcNow -lt $deadline) {
        [Threading.Thread]::Sleep(10)
    }
    Assert-Idle ($script:MetadataMonitor.Generation -gt $Before) 'Real filesystem/process event was not observed'
}
function Write-Fixture([string] $Path, [string] $Value) {
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Path))
    [IO.File]::WriteAllText($Path, $Value)
}

$temporary = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [IO.Path]::GetTempPath() }
$fixture = Join-Path $temporary ('control-panel-idle-' + [guid]::NewGuid().ToString('N'))
$root = Join-Path $fixture 'runtime'
$script:PackagesPath = Join-Path $root 'data/system/packages.list'
$script:AppWindowsFile = Join-Path $root 'app-windows.list'
$script:Scans = @{ State = 0; Apps = 0; Process = 0; List = 0 }
$script:Duties = @{ Action = 0; Update = 0; Activation = 0; Cancel = 0 }
$script:ViewState = @{ AppFiles = @() }
$script:LiveState = @{ Stamp = $null; AppsStamp = $null; PendingSince = $null; PendingFirst = $null; Tick = 0; WasVisible = $false; RecoveryAt = $null }
$script:LiveRefreshDebounceMs = 400
$script:LiveRefreshMaximumDelayMs = 2000
$script:LiveRefreshRecoveryMs = 5000
$script:ActionState = @{ Closed = $false }
$script:IsBusy = $false; $script:Draws = 0; $script:Notifications = 0
$script:CancelPending = $false; $script:ShowPending = $false
$script:MetadataMonitor = $null
$child = $null
$window = [Windows.Window]::new()
$window.Title = 'Gesher metadata lifecycle fixture'
$window.Width = 200; $window.Height = 100
try {
    Write-Fixture $script:PackagesPath "fixture.alpha`n"
    Initialize-PanelMetadata
    Assert-Idle ($null -ne $script:MetadataMonitor) 'Production dispatcher monitor did not initialize'
    $script:MetadataMonitor.add_Changed({ Assert-UiThread; $script:Notifications++ })
    Refresh-Dashboard
    $window.Show()
    Pump-Until { $script:LiveState.WasVisible -and -not $script:LiveState.PendingSince } 'Initial visible view did not settle'
    Assert-Idle ($script:ShownPackages -contains 'fixture.alpha') 'Initial application view is stale'
    Assert-Idle ($script:MetadataMonitor.WatcherCount -ge 1 -and $script:MetadataMonitor.WatcherCount -le 8) 'Watcher handle budget is invalid'

    $window.WindowState = [Windows.WindowState]::Minimized
    Pump-For 80
    Assert-Idle (-not (Test-PanelMetadataVisible)) 'Actual minimized WPF window was treated as visible'
    $before = Scan-Total
    $actions = $script:Duties.Action; $updates = $script:Duties.Update; $activations = $script:Duties.Activation
    $script:LiveState.RecoveryAt = [DateTime]::MinValue
    $script:CancelPending = $true
    Pump-For 650
    Assert-Idle ((Scan-Total) -eq $before) 'Minimized clean panel performed periodic metadata/PID/list scans'
    Assert-Idle ($script:Duties.Action -gt $actions -and $script:Duties.Update -gt $updates -and
        $script:Duties.Activation -gt $activations -and $script:Duties.Cancel -eq 1) 'Minimizing stopped action/activation/progress/cancel duties'

    $generation = $script:MetadataMonitor.Generation
    Write-Fixture $script:PackagesPath "fixture.alpha`nfixture.beta`n"
    Wait-ManagedNotification $generation
    Pump-For 650
    Assert-Idle ((Scan-Total) -eq $before -and $script:LiveState.PendingSince) 'Dirty minimized panel scanned metadata or lost its dirty state'
    $window.WindowState = [Windows.WindowState]::Normal
    Pump-Until { $script:ShownPackages -contains 'fixture.beta' -and -not $script:LiveState.PendingSince } 'Restore lost an external install'

    # A burst queues UI work; it cannot execute a PowerShell callback while the
    # dispatcher is blocked, and it must be coalesced before expensive refresh.
    $generation = $script:MetadataMonitor.Generation
    $callbacks = $script:Notifications
    # PowerShell variable names are case-insensitive: an unqualified $draws
    # here would alias the script's live counter, not capture its current value.
    $script:DrawsBeforeBurst = $script:Draws
    $shortcut = Join-Path $root 'data/data/fixture.beta/gesher-shell/shortcut.properties'
    for ($i = 0; $i -lt 100; $i++) { Write-Fixture $shortcut "label-$i" }
    Wait-ManagedNotification $generation
    Assert-Idle ($script:Notifications -eq $callbacks) 'A worker callback invoked PowerShell without WPF dispatch'
    Pump-Until { $script:ShownLabels -contains 'label-99' -and -not $script:LiveState.PendingSince } 'Debounced shortcut burst did not update the visible view'
    Assert-Idle (($script:Draws - $script:DrawsBeforeBurst) -le 3) 'Notification burst rebuilt the view without coalescing'

    $window.Hide()
    Pump-For 80
    Assert-Idle (-not (Test-PanelMetadataVisible)) 'Actual hidden WPF window was treated as visible'
    $before = Scan-Total
    Write-Fixture $script:PackagesPath "fixture.beta`n"
    Pump-For 650
    Assert-Idle ((Scan-Total) -eq $before) 'Hidden panel performed periodic metadata scans'
    $script:ShowPending = $true
    Pump-Until { (Test-PanelMetadataVisible) -and $script:ShownPackages.Count -eq 1 -and $script:ShownPackages[0] -eq 'fixture.beta' } 'Heartbeat activation failed to restore and refresh an external uninstall'

    # Actual OS process completion is observed without a periodic PID poll.
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = Join-Path $PSHOME 'powershell.exe'
    $info.Arguments = '-NoLogo -NoProfile -NonInteractive -Command "[Threading.Thread]::Sleep(30000)"'
    $info.UseShellExecute = $false; $info.CreateNoWindow = $true
    $child = [Diagnostics.Process]::Start($info)
    Write-Fixture (Join-Path $root 'data/system/gesher-device.ready') ([string]$child.Id)
    Pump-Until { $script:ShownDevice -and $script:MetadataMonitor.ProcessCount -eq 1 -and -not $script:LiveState.PendingSince } 'Device start did not establish a process observer'
    $script:LiveState.RecoveryAt = [DateTime]::UtcNow.AddSeconds(30)
    $generation = $script:MetadataMonitor.Generation
    $child.Kill()
    Assert-Idle ($child.WaitForExit(3000)) 'Owned process control did not terminate'
    Wait-ManagedNotification $generation
    Pump-Until { -not $script:ShownDevice -and $script:MetadataMonitor.ProcessCount -eq 0 } 'Real device exit did not refresh before fallback polling'
    $child.Dispose(); $child = $null

    # Exceed the external-path watch budget, then change an unwatched path.
    # The real file-stamp recovery must repair missed events while visible.
    $external = @()
    for ($i = 0; $i -lt 12; $i++) {
        $path = Join-Path $fixture "external-$i/icon.ico"
        Write-Fixture $path 'initial'
        $external += $path
    }
    $script:ViewState.AppFiles = $external
    Update-PanelMetadataWatch
    Assert-Idle ($script:MetadataMonitor.WatcherCount -eq 8) 'External files exceeded the fixed watcher handle budget'
    for ($i = 0; $i -lt 20; $i++) { Update-PanelMetadataWatch }
    Assert-Idle ($script:MetadataMonitor.WatcherCount -eq 8) 'Stable updates accumulated watcher handles'
    Pump-For 80
    # Keep this declared renderer boundary's file set for the recovery read.
    $script:LiveState.AppsStamp = Get-AppFilesStamp
    $script:LiveState.PendingSince = $null; $script:LiveState.PendingFirst = $null
    $script:LiveState.RecoveryAt = [DateTime]::UtcNow.AddSeconds(30)
    $script:DrawsBeforeRecovery = $script:Draws
    Write-Fixture $external[-1] 'missed event with changed length'
    Pump-For 100
    Assert-Idle ($script:Draws -eq $script:DrawsBeforeRecovery) 'The over-budget path unexpectedly triggered a scan before recovery'
    $script:LiveState.RecoveryAt = [DateTime]::MinValue
    Pump-Until { $script:Draws -gt $script:DrawsBeforeRecovery } 'Visible missed-event recovery did not repair an unwatched file'

    # Retiring the watcher must release its actual OS directory handles and
    # suppress already queued callbacks when the WPF dispatcher resumes.
    Pump-Until { -not $script:LiveState.PendingSince } 'Recovery view did not settle'
    $generation = $script:MetadataMonitor.Generation
    $callbacks = $script:Notifications
    Write-Fixture $script:PackagesPath "fixture.gamma`n"
    Wait-ManagedNotification $generation
    $ownedMonitor = $script:MetadataMonitor
    $script:ActionState.Closed = $true
    $before = Scan-Total
    $window.Close()
    Pump-For 100
    Assert-Idle ($script:Notifications -eq $callbacks -and (Scan-Total) -eq $before) 'Shutdown delivered a queued callback or ran another heartbeat'
    Assert-Idle ($ownedMonitor.WatcherCount -eq 0 -and $ownedMonitor.ProcessCount -eq 0 -and -not $ownedMonitor.Healthy) 'Shutdown retained metadata watcher/process handles'
    Remove-Item -LiteralPath $fixture -Recurse -Force
    Assert-Idle (-not (Test-Path -LiteralPath $fixture)) 'Disposed watchers retained temporary directories'
    Write-Host "PASS: control panel idle WPF lifecycle ($script:Checks checks, notifications=$script:Notifications, draws=$script:Draws)"
} finally {
    $script:ActionState.Closed = $true
    Close-PanelMetadata
    if ($child) {
        if (-not $child.HasExited) { $child.Kill(); [void]$child.WaitForExit(3000) }
        $child.Dispose()
    }
    if ($window.IsVisible) { $window.Close() }
    if (Test-Path -LiteralPath $fixture) { Remove-Item -LiteralPath $fixture -Recurse -Force }
}
