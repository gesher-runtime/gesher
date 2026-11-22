# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
# Real WPF layout and routed production buttons, with process launch/uninstall adapted.
param([string] $SourceRef, [switch] $Measure)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$path = Join-Path $repo 'tools/package/gesher-control-panel.ps1'
$source = if ($SourceRef) { (& git -C $repo show "${SourceRef}:tools/package/gesher-control-panel.ps1") -join "`n" } else { [IO.File]::ReadAllText($path) }
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseInput($source, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw ($errors | Out-String) }
$assignment = @($ast.FindAll({ param($node)
    $node -is [Management.Automation.Language.AssignmentStatementAst] -and $node.Left.Extent.Text -eq '[xml]$xaml'
}, $false))
if ($assignment.Count -ne 1) { throw 'Expected one production window XAML' }
$uiAst = [Management.Automation.Language.Parser]::ParseFile((Join-Path $repo 'tools/package/gesher-ui.ps1'), [ref]$tokens, [ref]$errors)
$textFunction = $uiAst.Find({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Get-GesherText' }, $false)
. ([scriptblock]::Create($textFunction.Extent.Text))
$script:GesherUiCatalog = Get-Content -LiteralPath (Join-Path $repo 'tools/package/ui-messages.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$begin = $source.IndexOf('# Attach click events through visual tree dispatcher')
$end = $source.IndexOf('# Live refresh: reflect devices', $begin)
if ($begin -lt 0 -or $end -le $begin) { throw 'Production routed-button boundary changed' }
$handler = [scriptblock]::Create($source.Substring($begin, $end - $begin))
$script:Checks = 0
function Assert-List([bool] $condition, [string] $message) {
    if (-not $condition) { throw $message }
    $script:Checks++
}
function Find-Visual($node, [type] $type) {
    if ($node -is $type) { $node }
    for ($i = 0; $i -lt [Windows.Media.VisualTreeHelper]::GetChildrenCount($node); $i++) {
        Find-Visual ([Windows.Media.VisualTreeHelper]::GetChild($node, $i)) $type
    }
}
function Settle-Layout {
    $window.UpdateLayout()
    $window.Dispatcher.Invoke([Action]{}, [Windows.Threading.DispatcherPriority]::Render)
    $window.Dispatcher.Invoke([Action]{}, [Windows.Threading.DispatcherPriority]::ApplicationIdle)
    $window.UpdateLayout()
}
function Count-Containers {
    $count = 0
    for ($i = 0; $i -lt $appsList.Items.Count; $i++) {
        if ($null -ne $appsList.ItemContainerGenerator.ContainerFromIndex($i)) { $count++ }
    }
    return $count
}
$script:OnAppLaunch = { param($pkg) $script:Launched = $pkg }
$script:OnAppUninstall = { param($pkg) $script:Uninstalled = $pkg }
$script:OnAppDetails = { param($pkg) $script:Detailed = $pkg }
foreach ($language in 'en', 'he') {
    foreach ($height in 600, 820) {
        $script:GesherUiLanguage = $language
        $flowDirection = if ($language -eq 'he') { 'RightToLeft' } else { 'LeftToRight' }
        . ([scriptblock]::Create($assignment[0].Extent.Text))
        $window = [Windows.Markup.XamlReader]::Load([Xml.XmlNodeReader]::new($xaml))
        try {
            $window.ShowInTaskbar = $false; $window.WindowStartupLocation = 'Manual'; $window.Left = -12000
            $window.Width = 800; $window.Height = $height
            $panelScroll = $window.FindName('PanelScroll'); $panelLayout = $window.FindName('PanelLayout')
            $appsList = $window.FindName('AppsList')
            $script:MeasuredList = $appsList
            $script:PeakContainers = 0
            $appsList.ItemContainerGenerator.add_StatusChanged({
                if ($script:MeasuredList.ItemContainerGenerator.Status -eq [Windows.Controls.Primitives.GeneratorStatus]::ContainersGenerated) {
                    $count = 0
                    for ($i = 0; $i -lt $script:MeasuredList.Items.Count; $i++) {
                        if ($null -ne $script:MeasuredList.ItemContainerGenerator.ContainerFromIndex($i)) { $count++ }
                    }
                    $script:PeakContainers = [Math]::Max($script:PeakContainers, $count)
                }
            })
            foreach ($name in 'BtnEnableMicrog', 'BtnInstallStore') { $window.FindName($name).Visibility = 'Collapsed' }
            . $handler
            $items = [Collections.Generic.List[object]]::new()
            for ($i = 0; $i -lt 1000; $i++) {
                $items.Add([pscustomobject]@{ Package = "fixture.$i"; Label = "App $i"; IconImage = $null })
            }
            $appsList.ItemsSource = $items
            $window.Show(); Settle-Layout
            $panelLayout.Height = [Math]::Max(640, $panelScroll.ActualHeight - 48)
            Settle-Layout
            $viewport = $window.FindName('AppsViewport')
            if (-not $viewport) { $viewport = $appsList.Template.FindName('AppsViewport', $appsList) }
            $initial = Count-Containers
            "Panel containers: language=$language height=$height items=1000 realized=$initial startup_peak=$script:PeakContainers viewport=$($viewport.ViewportHeight) extent=$($viewport.ExtentHeight)"
            if (-not $Measure) { Assert-List ($initial -gt 0 -and $initial -lt 100) 'Offscreen rows were eagerly constructed' }
            if (-not $Measure) { Assert-List ($script:PeakContainers -lt 100) 'Startup built the complete row tree before constraining its viewport' }
            Assert-List ($viewport.ViewportHeight -ge 36) 'List has no usable viewport'
            Assert-List ($viewport.ExtentHeight -gt $viewport.ViewportHeight) 'List is not scrollable'
            $viewport.ScrollToEnd(); Settle-Layout
            $last = $appsList.ItemContainerGenerator.ContainerFromIndex(999)
            Assert-List ($null -ne $last) 'Last app cannot be reached'
            $buttons = @(Find-Visual $last ([Windows.Controls.Button]))
            Assert-List ($buttons.Count -eq 3) 'Row actions missing'
            foreach ($button in $buttons) {
                Assert-List ($button.Tag -ceq 'fixture.999') 'Recycled button retained an old package'
                $button.RaiseEvent([Windows.RoutedEventArgs]::new([Windows.Controls.Button]::ClickEvent, $button))
            }
            Assert-List ($script:Launched -ceq 'fixture.999' -and $script:Uninstalled -ceq 'fixture.999' -and $script:Detailed -ceq 'fixture.999') 'Production routed actions used stale data'
            if (-not $Measure) { Assert-List ((Count-Containers) -lt 100) 'Scrolling retained the entire row tree' }
            $viewport.ScrollToTop(); Settle-Layout
            Assert-List ($null -ne $appsList.ItemContainerGenerator.ContainerFromIndex(0)) 'First app cannot be reached after recycling'
            $window.Height = if ($height -eq 600) { 820 } else { 600 }
            Settle-Layout
            $panelLayout.Height = [Math]::Max(640, $panelScroll.ActualHeight - 48)
            Settle-Layout
            $viewport.ScrollToEnd(); Settle-Layout
            Assert-List ($null -ne $appsList.ItemContainerGenerator.ContainerFromIndex(999)) 'Resize lost the final app'
            if (-not $Measure) { Assert-List ((Count-Containers) -lt 100) 'Resize retained the entire row tree' }
            $viewport.ScrollToTop(); Settle-Layout
            $appsList.ItemsSource = @([pscustomobject]@{ Package = 'replacement'; Label = 'Replacement'; IconImage = $null })
            Settle-Layout
            $replacement = $appsList.ItemContainerGenerator.ContainerFromIndex(0)
            $buttons = @(Find-Visual $replacement ([Windows.Controls.Button]))
            Assert-List ($buttons.Count -eq 3 -and @($buttons | Where-Object { $_.Tag -cne 'replacement' }).Count -eq 0) 'Refresh retained a removed package'
            $appsList.ItemsSource = @(); Settle-Layout
            Assert-List ((Count-Containers) -eq 0) 'Empty list retained row containers'
        } finally { $window.Close() }
    }
}
"PASS: control panel virtualization ($script:Checks checks)"
