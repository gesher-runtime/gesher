# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
# Presentation only. Call on the WPF dispatcher; caller owns events and lifetime.
function New-PanelView([ValidateSet('dashboard', 'confirm', 'message')] [string] $Name, [hashtable] $Resources) {
    $path = Join-Path $PSScriptRoot ("gesher-panel-{0}.xaml" -f $Name)
    $source = [IO.File]::ReadAllText($path, [Text.Encoding]::UTF8)
    [xml]$document = $source
    $reader = [System.Xml.XmlNodeReader]::new($document)
    try { $view = [System.Windows.Markup.XamlReader]::Load($reader) }
    finally { $reader.Close() }
    # Object resources reach template instances too, without parsing translated
    # braces, ampersands, tabs or Hebrew as XML or markup extensions.
    foreach ($match in [regex]::Matches($source, '\{DynamicResource Text_([A-Za-z0-9_]+)\}')) {
        $key = $match.Groups[1].Value
        $view.Resources["Text_$key"] = [string](Get-GesherText $key)
    }
    foreach ($key in $Resources.Keys) { $view.Resources[$key] = $Resources[$key] }
    return $view
}
