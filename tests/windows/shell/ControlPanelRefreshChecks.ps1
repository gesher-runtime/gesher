param([string] $PanelPath = (Join-Path $PSScriptRoot '../../../tools/package/gesher-control-panel.ps1'))

$ErrorActionPreference = 'Stop'
# Execute the production polling/refresh functions without creating a WPF window. The
# boundaries below model file reads and rendering; native WPF execution remains in Windows.
$tokens = $null; $parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile(
    [IO.Path]::GetFullPath($PanelPath), [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw ($parseErrors | Out-String) }
foreach ($name in 'Refresh-Dashboard', 'Invoke-LiveRefreshTick', 'Complete-DashboardAction') {
    $functions = @($ast.FindAll({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
    }, $false))
    if ($functions.Count -ne 1) { throw "Expected one production function: $name" }
    . ([scriptblock]::Create($functions[0].Extent.Text))
}

$script:Checks = 0
function Assert-Refresh([bool] $value, [string] $message) {
    if (-not $value) { throw $message }
    $script:Checks++
}
function Reset-Refresh {
    $script:IsBusy = $false
    $script:LiveRefreshDebounceMs = 400
    $script:LiveRefreshSlowEvery = 4
    $script:LiveState = @{ Stamp = 'state-a'; AppsStamp = 'apps-a'; PendingSince = $null; Tick = 0 }
    $script:StateStamp = 'state-a'
    $script:AppsStamp = 'apps-a'
    $script:Device = $false
    $script:ShownDevice = $false
    $script:Reads = 0
    $script:Draws = 0
    $script:FailRead = $false
    $script:ChangeAppsDuringRead = $false
}
function Get-StateStamp { $script:StateStamp }
function Get-AppFilesStamp { $script:AppsStamp }
function Get-DeviceStatus { $script:Device }
function Get-MicrogStatus { $false }
function Update-DeviceView([bool] $running) { $script:ShownDevice = $running }
function Update-MicrogView([bool] $enabled) { }
function Update-StoreView($installed) { }
function Get-InstalledApps {
    $script:Reads++
    if ($script:FailRead) { $script:FailRead = $false; throw 'Transient package read failure' }
    if ($script:ChangeAppsDuringRead) {
        $script:ChangeAppsDuringRead = $false
        $script:AppsStamp = 'apps-b'
    }
    @([PSCustomObject]@{ Package = 'fixture.app'; Label = 'Fixture'; IconPath = $null })
}
function Update-AppsView($installed) { $script:Draws++ }
function Set-Busy([bool] $busy) { $script:IsBusy = $busy }
function Age-PendingRefresh {
    $script:LiveState.PendingSince = [DateTime]::UtcNow.AddSeconds(-1)
}

Reset-Refresh
Invoke-LiveRefreshTick
Assert-Refresh ($script:Reads -eq 0) 'Unchanged state rebuilt the application list'
$script:StateStamp = 'state-b'
Invoke-LiveRefreshTick
Assert-Refresh ($script:Reads -eq 0 -and $null -ne $script:LiveState.PendingSince) 'File change bypassed debounce'
Age-PendingRefresh
Invoke-LiveRefreshTick
Assert-Refresh ($script:Draws -eq 1 -and $null -eq $script:LiveState.PendingSince) 'Stable change did not refresh'
Invoke-LiveRefreshTick
Assert-Refresh ($script:Draws -eq 1) 'Stable state caused a redundant redraw'

Reset-Refresh
$script:IsBusy = $true
$script:StateStamp = 'state-b'
Invoke-LiveRefreshTick
Assert-Refresh ($script:Reads -eq 0) 'Busy panel read state during an action'
$script:IsBusy = $false
Invoke-LiveRefreshTick
Age-PendingRefresh
Invoke-LiveRefreshTick
Assert-Refresh ($script:Draws -eq 1) 'Change during a busy action was lost'

Reset-Refresh
$script:StateStamp = 'state-b'
Invoke-LiveRefreshTick
Age-PendingRefresh
$script:FailRead = $true
$failed = $false
try { Invoke-LiveRefreshTick } catch {
    if ($_.Exception.Message -ne 'Transient package read failure') { throw }
    $failed = $true
}
Assert-Refresh $failed 'The transient failure control did not execute'
Assert-Refresh ($null -ne $script:LiveState.PendingSince) 'Transient read failure discarded the pending refresh'
Age-PendingRefresh
Invoke-LiveRefreshTick
Assert-Refresh ($script:Reads -eq 2 -and $script:Draws -eq 1) 'Stable files were never retried after a read failure'

Reset-Refresh
$script:IsBusy = $true
$script:FailRead = $true
$failed = $false
try { Complete-DashboardAction } catch {
    if ($_.Exception.Message -ne 'Transient package read failure') { throw }
    $failed = $true
}
Assert-Refresh $failed 'The action-completion failure control did not execute'
Assert-Refresh (-not $script:IsBusy) 'Refresh failure left the panel permanently busy'
Age-PendingRefresh
Invoke-LiveRefreshTick
Assert-Refresh ($script:Draws -eq 1) 'Timer could not retry after an action-completion failure'

Reset-Refresh
$script:ChangeAppsDuringRead = $true
Refresh-Dashboard
$script:LiveState.Tick = 3
Invoke-LiveRefreshTick
Assert-Refresh ($null -ne $script:LiveState.PendingSince) 'Shortcut/icon change during a read was acknowledged without being displayed'
Age-PendingRefresh
Invoke-LiveRefreshTick
Assert-Refresh ($script:Draws -eq 2) 'Shortcut/icon change during a read was not refreshed'

Reset-Refresh
$script:Device = $true
$script:LiveState.Tick = 3
Invoke-LiveRefreshTick
Assert-Refresh ($script:ShownDevice -and $script:Reads -eq 0) 'PID-only state change was not reflected independently of app reads'
"PASS: control panel refresh contracts ($script:Checks checks)"
