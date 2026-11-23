# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
param([string] $PackageDirectory = (Join-Path $PSScriptRoot '../../../tools/package'))
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName PresentationFramework
$package = [IO.Path]::GetFullPath($PackageDirectory)
. (Join-Path $package 'gesher-ui.ps1')
. (Join-Path $package 'gesher-panel-view.ps1')
$manifest = [IO.File]::ReadAllText((Join-Path $PSScriptRoot '../../../tools/package/gesher.iss'))
foreach ($file in 'gesher-panel-view.ps1','gesher-panel-dashboard.xaml','gesher-panel-confirm.xaml','gesher-panel-message.xaml') {
    if (-not (Test-Path -LiteralPath (Join-Path $package $file))) { throw "Missing resource: $file" }
    if (-not $manifest.Contains(('Source: "{0}"; DestDir: "{{app}}"; Flags: ignoreversion' -f $file))) { throw "Unpackaged resource: $file" }
}
foreach ($language in 'en','he') {
    $script:GesherUiLanguage = $language
    foreach ($name in 'dashboard','confirm','message') {
        $view = New-PanelView $name @{
            PanelFlowDirection = [System.Windows.FlowDirection]$(if ($language -eq 'he') { 'RightToLeft' } else { 'LeftToRight' })
            PanelBadgeBackground = [System.Windows.Media.Brushes]::Red
            PanelBadgeForeground = [System.Windows.Media.Brushes]::Blue
            PanelMessageSymbol = '!'
        }
        if ($name -eq 'dashboard') {
            if ($view.Title -cne (Get-GesherText 'cp_title')) { throw 'Title resource missing' }
            $list = $view.FindName('AppsList')
            $row = $list.ItemTemplate.LoadContent()
            if (-not $row) { throw 'Item template failed' }
            $literal = "{literal} & <text>`t`nHebrew"
            $view.Resources['Text_cp_title'] = $literal
            if ($view.Title -cne $literal) { throw 'Resource was interpreted as markup' }
        } elseif ($name -eq 'confirm') {
            $view.Resources['PanelConfirmStyle'] = $view.Resources['DangerActionButton']
            if ($view.FindName('BtnConfirm').Style -ne $view.Resources['DangerActionButton']) { throw 'Confirm style missing' }
        } else {
            if ($view.FindName('BtnOk').IsCancel -ne $true) { throw 'Cancel contract changed' }
        }
        $view.Close()
    }
}
foreach ($file in Get-ChildItem (Join-Path $package '*.ps1')) {
    $tokens = $null; $errors = $null
    $null = [System.Management.Automation.Language.Parser]::ParseFile($file.FullName, [ref]$tokens, [ref]$errors)
    if ($errors.Count) { throw ($errors | Out-String) }
}
'Panel presentation load and PowerShell parse checks passed (en/he).'
