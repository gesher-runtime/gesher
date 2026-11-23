param([string] $PanelPath = (Join-Path $PSScriptRoot '../../../tools/package/gesher-control-panel.ps1'))
$ErrorActionPreference = 'Stop'

# Deliberately omit notifications. Real production parsers, app projection,
# signatures, stamps and scheduling must recover every consumed metadata input.
# Only bitmap decoding and non-app rendering are declared fixture boundaries.
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile([IO.Path]::GetFullPath($PanelPath), [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw ($errors | Out-String) }
foreach ($name in 'Get-SeparateWindowApps','Get-InstalledApps','Get-FileStamp','Get-StateStamp','Get-AppFilesStamp',
        'Update-AppsView','Test-PanelMetadataVisible','Request-PanelMetadataRefresh','Update-PanelMetadataWatch',
        'Refresh-Dashboard','Invoke-LiveRefreshTick') {
    $definitions = @($ast.FindAll({ param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
    }, $false))
    if ($definitions.Count -ne 1) { throw "Expected one production function: $name" }
    . ([scriptblock]::Create($definitions[0].Extent.Text))
}
$script:Checks = 0
function Assert-Recovery([bool] $okay, [string] $reason) {
    if (-not $okay) { throw $reason }
    $script:Checks++
}
function Get-PanelAppIcon([string] $path, [string] $stamp, [hashtable] $icons) {
    Assert-Recovery ([IO.File]::Exists($path)) 'An unavailable candidate reached image decoding'
    if ([IO.File]::ReadAllText($path) -eq 'decoder-failure') { throw 'Declared decoder failure' }
    $image = [pscustomobject]@{ Path = $path }
    $icons[$path] = @{ Stamp = $stamp; Image = $image }
    return $image
}
function Update-DeviceView { }
function Update-MicrogView { }
function Update-StoreView { }
function Get-DeviceStatus { $false }
function Get-MicrogStatus { $false }
function Write-Metadata([string] $Path, [string] $Value) {
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Path))
    [IO.File]::WriteAllText($Path, $Value)
}
function Recover-Visible {
    $script:LiveState.RecoveryAt = [DateTime]::MinValue
    Invoke-LiveRefreshTick
    if ($script:LiveState.PendingSince) { $script:LiveState.PendingSince = [DateTime]::UtcNow.AddSeconds(-1) }
    Invoke-LiveRefreshTick
}

$fixture = Join-Path ([IO.Path]::GetTempPath()) ('panel-recovery-' + [guid]::NewGuid().ToString('N'))
$root = Join-Path $fixture 'runtime'
$packages = Join-Path $root 'data/system/packages.list'
$shortcut = Join-Path $root 'data/data/fixture.app/gesher-shell/shortcut.properties'
$conventional = Join-Path $root 'data/data/fixture.app/gesher-shell/icon.ico'
$external = Join-Path $fixture 'external/late.ico'
$custom = Join-Path $root 'custom/late.ico'
$script:AppWindowsFile = Join-Path $root 'data/system/app-windows.properties'
$icoFile = Join-Path $fixture 'panel-default.ico'
$appsList = [pscustomobject]@{ ItemsSource = @() }
$window = @{ IsVisible = $true; WindowState = 'Normal' }
$script:IsBusy = $false
$script:MetadataMonitor = $null
$script:ViewState = @{ Device = $false; AppFiles = @(); Icons = @{}; AppsSignature = $null }
$script:LiveState = @{ Stamp = $null; AppsStamp = $null; PendingSince = $null; PendingFirst = $null; Tick = 0; WasVisible = $true; RecoveryAt = $null }
$script:LiveRefreshDebounceMs = 400; $script:LiveRefreshMaximumDelayMs = 2000; $script:LiveRefreshRecoveryMs = 5000
try {
    # Each explicitly watched state input must also change the fallback stamp.
    foreach ($path in @((Join-Path $root 'data/system/gesher-device.ready'), $packages,
            (Join-Path $root 'microg.enabled'), (Join-Path $root 'system/priv-app/com.google.android.gms/base.apk'),
            $script:AppWindowsFile)) {
        $before = Get-StateStamp
        Write-Metadata $path 'metadata fixture'
        Assert-Recovery ($before -ne (Get-StateStamp)) "Creation is absent from recovery stamp: $path"
        $before = Get-StateStamp
        [IO.File]::Delete($path)
        Assert-Recovery ($before -ne (Get-StateStamp)) "Deletion is absent from recovery stamp: $path"
    }
    Write-Metadata $packages "fixture.app`n"
    Write-Metadata $script:AppWindowsFile "separate.fixture.app=false`n"
    Refresh-Dashboard; Refresh-Dashboard
    Assert-Recovery ($appsList.ItemsSource.Count -eq 1 -and -not $appsList.ItemsSource[0].SeparateWindows) 'Initial preference projection is invalid'
    Write-Metadata $script:AppWindowsFile "separate.fixture.app=true`n"
    Recover-Visible
    Assert-Recovery $appsList.ItemsSource[0].SeparateWindows 'Missed preference event left the actual app projection stale'
    [IO.File]::Delete($script:AppWindowsFile)
    Recover-Visible
    Assert-Recovery (-not $appsList.ItemsSource[0].SeparateWindows) 'Missed preference deletion retained the old separate-window state'

    Write-Metadata $shortcut "label=Fixture`nicon=$external`n"
    Write-Metadata $conventional 'declared conventional image'
    Recover-Visible
    Assert-Recovery ($appsList.ItemsSource[0].IconImage.Path -eq $conventional) 'Missing declared icon displaced the existing fallback'
    $parsed = Get-InstalledApps
    Assert-Recovery ($parsed[0].IconCandidates -contains $external -and $parsed[0].IconPath -eq $conventional) 'Declared candidate and renderable icon were not kept separate'
    Assert-Recovery ($script:ViewState.AppFiles -contains $external) 'Missing external candidate was omitted from observation/recovery'
    $shortcutStamp = Get-FileStamp $shortcut
    Write-Metadata $external 'declared external image'
    Recover-Visible
    Assert-Recovery ($appsList.ItemsSource[0].IconImage.Path -eq $external -and (Get-FileStamp $shortcut) -eq $shortcutStamp) 'External icon creation required a shortcut rewrite or restore'
    [IO.File]::Delete($external)
    Recover-Visible
    Assert-Recovery ($appsList.ItemsSource[0].IconImage.Path -eq $conventional -and $script:ViewState.AppFiles -contains $external) 'Icon deletion lost fallback or future observation'

    Write-Metadata $shortcut "label=Fixture`nicon=$custom`n"
    Recover-Visible
    Assert-Recovery ($script:ViewState.AppFiles -contains $custom) 'Nonconventional in-tree candidate was not observed while absent'
    Write-Metadata $custom 'declared custom image'
    Recover-Visible
    Assert-Recovery ($appsList.ItemsSource[0].IconImage.Path -eq $custom) 'Nonconventional icon creation was not recovered'
    Write-Metadata $custom 'decoder-failure'
    Recover-Visible
    Assert-Recovery ($null -eq $appsList.ItemsSource[0].IconImage -and $script:ViewState.AppFiles -contains $custom) 'Decoder failure became a successful image or discarded observation'
    Write-Metadata $custom 'repaired declared custom image'
    Recover-Visible
    Assert-Recovery ($appsList.ItemsSource[0].IconImage.Path -eq $custom) 'Replacing an undecodable candidate did not retry image selection'

    [IO.File]::Delete($custom); [IO.File]::Delete($conventional)
    Recover-Visible
    Assert-Recovery ($null -eq $appsList.ItemsSource[0].IconImage -and $script:ViewState.AppFiles -contains $icoFile) 'Absent default icon was not observed'
    Write-Metadata $icoFile 'declared default image'
    Recover-Visible
    Assert-Recovery ($appsList.ItemsSource[0].IconImage.Path -eq $icoFile) 'Late default icon creation was not recovered'
    Write-Metadata $packages ''
    Recover-Visible
    Assert-Recovery ($appsList.ItemsSource.Count -eq 0 -and $script:ViewState.Icons.Count -eq 0 -and
        $script:ViewState.AppFiles -notcontains $custom) 'Uninstall retained the removed app icon or observation paths'
    Write-Host "PASS: control panel recovery inputs ($script:Checks checks, production projection, notifications absent)"
} finally { if (Test-Path -LiteralPath $fixture) { Remove-Item -LiteralPath $fixture -Recurse -Force } }
