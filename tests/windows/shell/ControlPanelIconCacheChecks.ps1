# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
param(
    [string] $PanelPath = (Join-Path $PSScriptRoot '../../../tools/package/gesher-control-panel.ps1'),
    [string] $SourceRef,
    [switch] $Measure
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase, System.Drawing
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$source = if ($SourceRef) { (& git -C $repo show "${SourceRef}:tools/package/gesher-control-panel.ps1") -join "`n" } else {
    [IO.File]::ReadAllText([IO.Path]::GetFullPath($PanelPath))
}
$tokens = $null; $parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseInput($source, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw ($parseErrors | Out-String) }
foreach ($name in 'Get-FileStamp', 'Get-AppFilesStamp', 'Get-PanelAppIcon', 'Update-AppsView', 'Get-SeparateWindowApps') {
    $definitions = @($ast.FindAll({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
    }, $false))
    if ($definitions.Count -eq 0 -and $SourceRef -and $name -eq 'Get-PanelAppIcon') { continue }
    if ($definitions.Count -eq 0 -and $SourceRef -and $name -eq 'Get-SeparateWindowApps') { continue }
    if ($definitions.Count -ne 1) { throw "Expected one production function: $name" }
    $definition = $definitions[0].Extent.Text
    if ($name -eq 'Get-FileStamp') { $definition = $definition.Replace('function Get-FileStamp(', 'function Get-UncountedFileStamp(') }
    . ([scriptblock]::Create($definition))
}

$script:Stats = 0
function Get-FileStamp([string] $path) { $script:Stats++; Get-UncountedFileStamp $path }
$script:Checks = 0
function Assert-Icons([bool] $value, [string] $message) {
    if (-not $value) { throw $message }
    $script:Checks++
}
function Write-TestIcon([string] $path, [Drawing.Color] $color) {
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))
    $bitmap = [Drawing.Bitmap]::new(2, 2)
    try {
        for ($y = 0; $y -lt 2; $y++) { for ($x = 0; $x -lt 2; $x++) { $bitmap.SetPixel($x, $y, $color) } }
        $bitmap.Save($path, [Drawing.Imaging.ImageFormat]::Png)
    } finally { $bitmap.Dispose() }
}
function Get-IconColor($image) {
    $converted = [Windows.Media.Imaging.FormatConvertedBitmap]::new($image, [Windows.Media.PixelFormats]::Bgra32, $null, 0)
    $pixel = [byte[]]::new(4)
    $converted.CopyPixels([Windows.Int32Rect]::new(0, 0, 1, 1), $pixel, 4, 0)
    return "$($pixel[0]),$($pixel[1]),$($pixel[2]),$($pixel[3])"
}

$scratchRoot = [IO.Path]::GetFullPath((Join-Path $repo 'out')) + [IO.Path]::DirectorySeparatorChar
$scratch = Join-Path $scratchRoot ('gesher-panel-icons-' + [Guid]::NewGuid().ToString('N'))
$root = $scratch
# The per-app windows setting (production reader; no file: no app has separate windows).
$script:AppWindowsFile = Join-Path $root 'system/app-windows.properties'
$icoFile = Join-Path $root 'gesher.ico'
$appsList = [PSCustomObject]@{ ItemsSource = $null }
$script:ViewState = @{ AppFiles = @(); Icons = @{}; AppsSignature = $null }
try {
    Write-TestIcon $icoFile ([Drawing.Color]::Blue)
    $installed = [Collections.Generic.List[PSObject]]::new()
    for ($i = 0; $i -lt 100; $i++) {
        $path = Join-Path $root "data/data/fixture.$i/gesher-shell/icon.ico"
        Write-TestIcon $path ([Drawing.Color]::Red)
        $installed.Add([PSCustomObject]@{ Package = "fixture.$i"; Label = "App $i"; IconPath = $path })
    }
    for ($i = 0; $i -lt 20; $i++) {
        $installed.Add([PSCustomObject]@{ Package = "fallback.$i"; Label = "Fallback $i"; IconPath = $null })
    }
    Update-AppsView $installed
    Assert-Icons ($appsList.ItemsSource.Count -eq 120) 'Apps missing after first rendering'
    Assert-Icons ((Get-IconColor $appsList.ItemsSource[0].IconImage) -eq '0,0,255,255') 'Initial red icon did not decode'
    Assert-Icons ($appsList.ItemsSource[0].IconImage.IsFrozen) 'Published icon is not frozen'
    $initial = @($appsList.ItemsSource)
    $script:Stats = 0
    $null = Get-AppFilesStamp
    $pollStats = $script:Stats

    # One label change should retain every unchanged decoded icon object.
    $installed[0].Label = 'Renamed app'
    Update-AppsView $installed
    $reused = @((0..119) | Where-Object { [Object]::ReferenceEquals($initial[$_].IconImage, $appsList.ItemsSource[$_].IconImage) }).Count
    "Panel icon measurement: apps=120 unchanged_images_reused=$reused watched_file_stats_per_poll=$pollStats"
    if (-not $Measure) {
        Assert-Icons ($reused -eq 120) 'Label-only refresh decoded unchanged icons again'
        Assert-Icons ($pollStats -eq 241) 'Polling repeated file stats for duplicate icon paths'
        Assert-Icons ([Object]::ReferenceEquals($appsList.ItemsSource[100].IconImage, $appsList.ItemsSource[119].IconImage)) 'Default icons were decoded separately'
        Assert-Icons ($script:ViewState.Icons.Count -eq 101) 'Icon cache does not match current unique icon set'
    }

    # Replace bytes at the identical URI: the WPF URI cache must not hide changes.
    $changed = $installed[0].IconPath
    Write-TestIcon $changed ([Drawing.Color]::Lime)
    [IO.File]::SetLastWriteTimeUtc($changed, [DateTime]::UtcNow.AddSeconds(10))
    Update-AppsView $installed
    $changedColor = Get-IconColor $appsList.ItemsSource[0].IconImage
    "Panel changed-icon pixel=$changedColor"
    if (-not $Measure) {
        Assert-Icons ($changedColor -eq '0,255,0,255') 'Changed icon still shows WPF-cached old pixels'
        Assert-Icons (-not [Object]::ReferenceEquals($initial[0].IconImage, $appsList.ItemsSource[0].IconImage)) 'Changed icon retained its old image'
        Assert-Icons ([Object]::ReferenceEquals($initial[1].IconImage, $appsList.ItemsSource[1].IconImage)) 'One icon update decoded an unrelated icon'
    }

    # OnLoad must release the input file, and removed apps must release cache entries.
    $exclusive = [IO.File]::Open($changed, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    $exclusive.Dispose()
    $installed.RemoveAt(0)
    Update-AppsView $installed
    if (-not $Measure) {
        Assert-Icons ($script:ViewState.Icons.Count -eq 100 -and -not $script:ViewState.Icons.ContainsKey($changed)) 'Removed app retained its cached image'
        Write-TestIcon $icoFile ([Drawing.Color]::Lime)
        [IO.File]::SetLastWriteTimeUtc($icoFile, [DateTime]::UtcNow.AddSeconds(20))
        Update-AppsView $installed
        Assert-Icons ((Get-IconColor $appsList.ItemsSource[99].IconImage) -eq '0,255,0,255') 'Default-icon replacement was not detected'
    }
    Update-AppsView @()
    if (-not $Measure) { Assert-Icons ($script:ViewState.Icons.Count -eq 0) 'Empty app list retained cached icon images' }
    "PASS: control panel icon reuse contracts ($script:Checks checks)"
} finally {
    # The target is the exact GUID directory created above, never a computed parent.
    if (([IO.Path]::GetFullPath($scratch)).StartsWith($scratchRoot, [StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
    }
}
