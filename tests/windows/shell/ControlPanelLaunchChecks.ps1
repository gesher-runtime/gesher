# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
# The Control Center offers "Launch" only for packages with a MAIN/LAUNCHER activity, as a
# launcher lists them: AppShell records a package without one in
# data/data/<package>/gesher-shell/no-launcher-activity (com.gesher.camera, for example).
param(
    [string] $PanelPath = (Join-Path $PSScriptRoot '../../../tools/package/gesher-control-panel.ps1')
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$source = [IO.File]::ReadAllText([IO.Path]::GetFullPath($PanelPath))
$tokens = $null; $parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseInput($source, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw ($parseErrors | Out-String) }
foreach ($name in 'Get-FileStamp', 'Get-PanelAppIcon', 'Get-SeparateWindowApps', 'Get-InstalledApps', 'Update-AppsView',
        'ConvertTo-ProxyValue', 'Read-ProxyProperties', 'ConvertFrom-PropertiesEscapes', 'Get-ProxySettings', 'Get-ProxyDisplayText') {
    $definitions = @($ast.FindAll({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
    }, $false))
    if ($definitions.Count -ne 1) { throw "Expected one production function: $name" }
    . ([scriptblock]::Create($definitions[0].Extent.Text))
}

$script:Checks = 0
function Assert-Launch([bool] $value, [string] $message) {
    if (-not $value) { throw $message }
    $script:Checks++
}

# The production row template lives in the dashboard resource. Verify its
# localized button and bindings without depending on attribute order.
[xml]$dashboard = [IO.File]::ReadAllText((Join-Path (Split-Path ([IO.Path]::GetFullPath($PanelPath))) 'gesher-panel-dashboard.xaml'))
$launchButtons = @($dashboard.SelectNodes('//*[local-name()="Button"]') | Where-Object {
    $_.GetAttribute('Content') -eq '{DynamicResource Text_cp_btn_launch}'
})
Assert-Launch ($launchButtons.Count -eq 1) 'The dashboard must define one localized Launch button'
Assert-Launch ($launchButtons[0].GetAttribute('Tag') -eq '{Binding Package}' -and
    $launchButtons[0].GetAttribute('Visibility') -eq '{Binding LaunchVisibility}') 'The Launch button is not bound to its package and LaunchVisibility'

$scratchRoot = [IO.Path]::GetFullPath((Join-Path $repo 'out')) + [IO.Path]::DirectorySeparatorChar
$scratch = Join-Path $scratchRoot ('gesher-panel-launch-' + [Guid]::NewGuid().ToString('N'))
$root = $scratch
$script:AppWindowsFile = Join-Path $root 'system/app-windows.properties'
$script:ProxyFile = Join-Path $root 'data/system/proxy.properties'
$icoFile = Join-Path $root 'gesher.ico'
$appsList = [PSCustomObject]@{ ItemsSource = $null }
$script:ViewState = @{ AppFiles = @(); Icons = @{}; AppsSignature = $null }
try {
    [void][IO.Directory]::CreateDirectory((Join-Path $root 'data/system'))
    [IO.File]::WriteAllText((Join-Path $root 'data/system/packages.list'),
        "com.example.app`t/data/app/com.example.app-1`t1`t1`ncom.gesher.camera`t/data/app/com.gesher.camera-1`t1`t1`n")
    $marker = Join-Path $root 'data/data/com.gesher.camera/gesher-shell/no-launcher-activity'
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($marker))
    [IO.File]::WriteAllBytes($marker, [byte[]]@())

    $installed = Get-InstalledApps
    Assert-Launch ($installed.Count -eq 2) 'Both packages are listed'
    Update-AppsView $installed
    $app = $appsList.ItemsSource | Where-Object Package -eq 'com.example.app'
    $camera = $appsList.ItemsSource | Where-Object Package -eq 'com.gesher.camera'
    Assert-Launch ($app.LaunchVisibility -eq [System.Windows.Visibility]::Visible) 'A package with a launcher activity lost Launch'
    Assert-Launch ($camera.LaunchVisibility -eq [System.Windows.Visibility]::Collapsed) 'A package without a launcher activity offers Launch'
    Assert-Launch ($script:ViewState.AppFiles -contains $marker) 'The marker is not watched for live refresh'

    # The package gains a launcher activity (an update): AppShell removes the marker.
    Remove-Item -LiteralPath $marker
    Update-AppsView (Get-InstalledApps)
    $camera = $appsList.ItemsSource | Where-Object Package -eq 'com.gesher.camera'
    Assert-Launch ($camera.LaunchVisibility -eq [System.Windows.Visibility]::Visible) 'Launch did not come back once the marker was removed'
    "PASS: control panel launch contracts ($script:Checks checks)"
} finally {
    if (([IO.Path]::GetFullPath($scratch)).StartsWith($scratchRoot, [StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
    }
}
