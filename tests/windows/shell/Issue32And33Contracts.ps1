# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
# Exercise production dialog wrappers and, on Windows, the actual WPF windows.
# APK verification has Java behavior tests; source matches cannot verify signatures.
param(
    [string] $PanelPath = (Join-Path $PSScriptRoot '../../../tools/package/gesher-control-panel.ps1'),
    [string] $UiMessagesPath = (Join-Path $PSScriptRoot '../../../tools/package/ui-messages.json')
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2
$script:DialogChecks = 0
function Assert-Dialog([bool] $condition, [string] $message) {
    if (-not $condition) { throw "FAIL: $message" }
    $script:DialogChecks++
}

$tokens = $null; $parseErrors = $null
$panelAst = [Management.Automation.Language.Parser]::ParseFile($PanelPath, [ref]$tokens, [ref]$parseErrors)
Assert-Dialog ($parseErrors.Count -eq 0) 'Control panel parses without errors'
$uiAst = [Management.Automation.Language.Parser]::ParseFile(
    (Join-Path (Split-Path -Parent $PanelPath) 'gesher-ui.ps1'), [ref]$tokens, [ref]$parseErrors)
Assert-Dialog ($parseErrors.Count -eq 0) 'UI helper parses without errors'
$textFunction = $uiAst.Find({ param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Get-GesherText'
}, $false)
. ([scriptblock]::Create($textFunction.Extent.Text))
$script:GesherUiCatalog = Get-Content -LiteralPath $UiMessagesPath -Raw -Encoding UTF8 | ConvertFrom-Json
$script:DialogFunctions = @{}
foreach ($name in 'Show-UninstallConfirmDialog', 'Confirm-PanelUpdateInstallation', 'Show-ModernConfirmDialog', 'Show-ModernMessageDialog') {
    $definition = $panelAst.Find({ param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
    }, $false)
    Assert-Dialog ($null -ne $definition) "Production function exists: $name"
    $script:DialogFunctions[$name] = $definition.Extent.Text
    . ([scriptblock]::Create($definition.Extent.Text))
}

# Execute the real wrappers; replace only their modal boundary. This detects
# invalid argument expressions, lost results and incorrect parameter forwarding.
function Show-ModernConfirmDialog {
    param([string] $Title, [string] $Message, [string] $Detail,
        [string] $ConfirmText, [string] $CancelText, [switch] $IsDanger,
        [switch] $IsWarning, $IconImage, [string] $SubTitle)
    $script:ForwardedDialog = @{} + $PSBoundParameters
    return $script:ApproveDialog
}
$literalLabel = 'Probe & <tag> "{1}"'
$literalMessage = '{literal} & <tag> "{1}"' + [char]13 + [char]10 + 'second' + [char]9 + 'tabbed' + [char]10 + 'third'
$iconMarker = [object]::new()
foreach ($language in 'en', 'he') {
    $script:GesherUiLanguage = $language
    foreach ($approved in $true, $false) {
        $script:ApproveDialog = $approved
        foreach ($label in $literalLabel, 'gesher.test.dialog') {
            $result = @(Show-UninstallConfirmDialog -pkg 'gesher.test.dialog' -label $label -iconImage $iconMarker)
            Assert-Dialog ($result.Count -eq 1 -and $result[0] -is [bool] -and $result[0] -eq $approved) "Uninstall returns only the modal decision ($language/$approved)"
            $forwarded = $script:ForwardedDialog
            Assert-Dialog ($forwarded.Title -eq (Get-GesherText 'cp_uninstall_title')) 'Localized uninstall title'
            Assert-Dialog ($forwarded.Message -eq (Get-GesherText 'cp_uninstall_confirm' @($label))) 'App label and placeholder-like text remain literal'
            Assert-Dialog ($forwarded.Detail -eq (Get-GesherText 'cp_uninstall_detail')) 'Localized data-removal explanation'
            Assert-Dialog ($forwarded.ConfirmText -eq (Get-GesherText 'cp_btn_uninstall') -and $forwarded.CancelText -eq (Get-GesherText 'cp_update_cancel')) 'Localized action buttons'
            Assert-Dialog ([bool]$forwarded.IsDanger -and [object]::ReferenceEquals($forwarded.IconImage, $iconMarker)) 'Danger styling and original app icon are forwarded'
            $expectedSubtitle = if ($label -eq 'gesher.test.dialog') { '' } else { 'gesher.test.dialog' }
            Assert-Dialog ([string]$forwarded.SubTitle -eq $expectedSubtitle) 'Subtitle appears only when different from label'
        }
        $script:UpdateState = @{ Release = @{ Version = '1.2.3' } }
        $result = @(Confirm-PanelUpdateInstallation)
        Assert-Dialog ($result.Count -eq 1 -and $result[0] -is [bool] -and $result[0] -eq $approved) "Update returns only the modal decision ($language/$approved)"
        Assert-Dialog ($script:ForwardedDialog.Message -eq (Get-GesherText 'cp_update_confirm' @('1.2.3'))) 'Update version reaches the confirmation message'
        Assert-Dialog ([bool]$script:ForwardedDialog.IsWarning) 'Update retains warning styling'
    }
}
Write-Host "PASS: $script:DialogChecks production dialog wrapper checks"

# Run the actual XAML preparation and post-load text assignments. Only WPF
# construction is modeled, using controls named by the production XAML itself.
function Test-PreparedDialogText([string] $FunctionName, [bool] $OptionalText) {
    $definition = $panelAst.Find({ param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $FunctionName
    }, $false)
    $statements = @($definition.Body.EndBlock.Statements)
    $readerAt = -1; $loadAt = -1; $ownerAt = -1
    for ($index = 0; $index -lt $statements.Count; $index++) {
        if ($statements[$index] -is [Management.Automation.Language.AssignmentStatementAst]) {
            switch ($statements[$index].Left.Extent.Text) {
                '$reader' { $readerAt = $index }
                '$dialog' { $loadAt = $index }
                '$dialog.Owner' { $ownerAt = $index }
            }
        }
    }
    Assert-Dialog ($loadAt -gt 0 -and $ownerAt -gt $loadAt + 1) 'Production text setup is between XAML loading and owner assignment'
    $Title = $literalMessage; $Message = $literalMessage
    $Detail = if ($OptionalText) { $literalMessage } else { $null }
    $SubTitle = if ($OptionalText) { $literalMessage } else { $null }
    $ConfirmText = $literalMessage; $CancelText = $literalMessage
    $IsDanger = $true; $IsWarning = $false; $IsError = $OptionalText
    $flowDirection = if ($script:GesherUiLanguage -eq 'he') { 'RightToLeft' } else { 'LeftToRight' }
    . ([scriptblock]::Create(($statements[0..($loadAt - 1)] | ForEach-Object { $_.Extent.Text }) -join [Environment]::NewLine))
    $viewName = if ($FunctionName -eq 'Show-ModernConfirmDialog') { 'confirm' } else { 'message' }
    [xml]$dialogXaml = [IO.File]::ReadAllText((Join-Path (Split-Path -Parent $PanelPath) "gesher-panel-$viewName.xaml"))
    $controls = @{}
    foreach ($element in $dialogXaml.SelectNodes('//*[@Name]')) {
        $controls[$element.GetAttribute('Name')] = [pscustomobject]@{
            Text = $element.GetAttribute('Text')
            Content = $element.GetAttribute('Content')
            Visibility = $element.GetAttribute('Visibility')
        }
    }
    $dialog = [pscustomobject]@{ Title = $dialogXaml.DocumentElement.GetAttribute('Title'); Controls = $controls; Resources = @{} }
    $dialog | Add-Member -MemberType ScriptMethod -Name FindName -Value { param($name) $this.Controls[$name] }
    . ([scriptblock]::Create(($statements[($loadAt + 1)..($ownerAt - 1)] | ForEach-Object { $_.Extent.Text }) -join [Environment]::NewLine))
    Assert-Dialog ($dialog.Title -ceq $Title) 'Dialog title retains literal braces, XML characters and all whitespace'
    Assert-Dialog ($dialog.FindName('DialogMessage').Text -ceq $Message) 'Dialog message retains CRLF, LF, tab and literal markup'
    if ($FunctionName -eq 'Show-ModernConfirmDialog') {
        Assert-Dialog ($dialog.FindName('DialogTitle').Text -ceq $Title) 'Confirmation heading retains literal text'
        Assert-Dialog ($dialog.FindName('BtnConfirm').Content -ceq $ConfirmText -and $dialog.FindName('BtnCancel').Content -ceq $CancelText) 'Both action labels retain literal text'
        foreach ($name in 'DialogSubTitle', 'DialogDetail') {
            $control = $dialog.FindName($name)
            if ($OptionalText) {
                Assert-Dialog ($control.Text -ceq $literalMessage -and $control.Visibility -eq 'Visible') 'Optional dialog text is exact and visible'
            } else {
                Assert-Dialog ($control.Text -eq '' -and $control.Visibility -eq 'Collapsed') 'Missing optional dialog text stays collapsed'
            }
        }
    } else {
        Assert-Dialog ($dialog.FindName('BtnOk').Content -ceq (Get-GesherText 'cp_dialog_ok')) 'Message action retains the actual localized label'
    }
}
$beforeTextChecks = $script:DialogChecks
foreach ($language in 'en', 'he') {
    $script:GesherUiLanguage = $language
    foreach ($optional in $true, $false) {
        Test-PreparedDialogText 'Show-ModernConfirmDialog' $optional
        Test-PreparedDialogText 'Show-ModernMessageDialog' $optional
    }
}
Write-Host "PASS: $($script:DialogChecks - $beforeTextChecks) production dialog text preparation checks"

if ($env:OS -ne 'Windows_NT') {
    Write-Host 'SKIP: actual WPF interactions require Windows; the Windows CI gates run them.'
    return
}
# CI runs this script in its own fresh STA PowerShell step with an external
# two-minute timeout. That bound also covers a blocked dispatcher/Click handler.
Assert-Dialog ([Threading.Thread]::CurrentThread.ApartmentState -eq 'STA') 'WPF checks run in STA'
Write-Host 'WPF PHASE: loading assemblies'
Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase, System.Xaml, UIAutomationProvider, UIAutomationTypes
. (Join-Path (Split-Path -Parent $PanelPath) 'gesher-panel-view.ps1')
. ([scriptblock]::Create($script:DialogFunctions['Show-ModernConfirmDialog']))

$references = @(
    [System.Windows.Window].Assembly.Location,
    [System.Windows.Media.Visual].Assembly.Location,
    [System.Windows.DependencyObject].Assembly.Location,
    [System.Windows.Markup.IQueryAmbient].Assembly.Location,
    [System.Windows.Automation.Provider.IInvokeProvider].Assembly.Location,
    [System.Windows.Automation.AutomationElementIdentifiers].Assembly.Location
)
Write-Host 'WPF PHASE: compiling the dispatcher driver'
Add-Type -Path (Join-Path $PSScriptRoot 'WpfDialogProbe.cs') -ReferencedAssemblies $references

function Invoke-WpfDialogCase {
    param([scriptblock] $Show, [ValidateSet('Confirm', 'Cancel', 'Close', 'Ok')] [string] $Action,
        [string] $ExpectedTitle, [string] $ExpectedText, [switch] $MessageDialog)
    Write-Host "WPF CASE START: $script:GesherUiLanguage / $Action / $ExpectedTitle"
    $probe = [Gesher.Tests.WpfDialogProbe]::new($script:window, $Action, [bool]$MessageDialog)
    try {
        $probe.Start()
        $result = @(& $Show)
        if ($probe.Failure) { throw $probe.Failure }
        # Assertions run after the modal closes, outside the dispatcher callback.
        Assert-Dialog $probe.Observed 'Production function displayed a modal'
        Assert-Dialog ($probe.VisibleWindowCount -eq 1) 'Exactly one production modal opens'
        Assert-Dialog $probe.OwnerMatches 'Modal belongs to the control panel'
        Assert-Dialog ($probe.FlowDirection -eq $script:flowDirection) 'Production reading direction matches selected language'
        Assert-Dialog ($probe.Title -eq $ExpectedTitle) 'Literal title survives production XAML'
        Assert-Dialog ($probe.Texts.Contains($ExpectedText)) 'Literal message and XML characters survive production XAML'
        if ($MessageDialog) {
            Assert-Dialog ($probe.OkText -eq (Get-GesherText 'cp_dialog_ok') -and $probe.OkIsCancel) 'Message has a localized dismiss button'
            Assert-Dialog ($result.Count -eq 0) 'Message dialog does not leak return values'
        } else {
            Assert-Dialog $probe.CancelIsCancel 'Cancel remains the WPF cancel action'
            Assert-Dialog ($probe.CancelText -eq (Get-GesherText 'cp_update_cancel')) 'Cancel label is localized'
            Assert-Dialog ($result.Count -eq 1 -and $result[0] -is [bool]) 'Confirmation produces exactly one boolean'
            Assert-Dialog ($result[0] -eq ($Action -eq 'Confirm')) 'Only explicit confirmation approves the action'
        }
    } finally {
        $probe.Dispose()
    }
    Write-Host "WPF CASE PASS: $script:GesherUiLanguage / $Action / $ExpectedTitle"
}

Write-Host 'WPF PHASE: creating the owner window'
$script:window = [System.Windows.Window]::new()
$window.ShowInTaskbar = $false
$window.ShowActivated = $false
$window.Opacity = 0
$window.Width = 80
$window.Height = 80
try {
    Write-Host 'WPF PHASE: showing the owner window'
    $window.Show()
    Write-Host 'WPF PHASE: owner ready'
    foreach ($language in 'en', 'he') {
        $script:GesherUiLanguage = $language
        $script:flowDirection = if ($language -eq 'he') { 'RightToLeft' } else { 'LeftToRight' }
        foreach ($action in 'Confirm', 'Cancel', 'Close') {
            $fixture = @{ Action = $action; ExpectedTitle = (Get-GesherText 'cp_uninstall_title'); ExpectedText = (Get-GesherText 'cp_uninstall_confirm' @($literalLabel)) }
            Invoke-WpfDialogCase @fixture -Show { Show-UninstallConfirmDialog -pkg 'gesher.test.dialog' -label $literalLabel }
        }
        foreach ($action in 'Confirm', 'Cancel') {
            $fixture = @{ Action = $action; ExpectedTitle = (Get-GesherText 'cp_update_title'); ExpectedText = (Get-GesherText 'cp_update_confirm' @('1.2.3')) }
            Invoke-WpfDialogCase @fixture -Show { Confirm-PanelUpdateInstallation }
        }
        foreach ($isError in $true, $false) {
            Invoke-WpfDialogCase -Action Ok -MessageDialog -ExpectedTitle $literalMessage -ExpectedText $literalMessage -Show { Show-ModernMessageDialog -Title $literalMessage -Message $literalMessage -IsError $isError }
        }
    }
} finally {
    $window.Close()
}
Write-Host "PASS: $script:DialogChecks production wrapper and WPF interaction checks"
