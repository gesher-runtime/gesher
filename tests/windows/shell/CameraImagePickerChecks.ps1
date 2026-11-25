# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
param([string] $PanelPath = (Join-Path $PSScriptRoot '../../../tools/package/gesher-control-panel.ps1'))

$ErrorActionPreference = 'Stop'
$resolvedPanel = [IO.Path]::GetFullPath($PanelPath)
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($resolvedPanel, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw ($errors | Out-String) }

foreach ($name in 'Get-CameraImagePickerEnabled', 'Set-CameraImagePickerEnabled') {
    $definition = @($ast.FindAll({ param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
    }, $false))
    if ($definition.Count -ne 1) { throw "Expected one production function: $name" }
    . ([scriptblock]::Create($definition[0].Extent.Text))
}

$checks = 0
function Assert-CameraCheck([bool] $condition, [string] $message) {
    if (-not $condition) { throw $message }
    $script:checks++
}

$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ('gesher-camera-picker-ps-' + [guid]::NewGuid().ToString('N'))
try {
    $null = New-Item -ItemType Directory -Path $tempRoot -Force

    # Null/empty path returns false.
    $script:CameraFile = $null
    Assert-CameraCheck (-not (Get-CameraImagePickerEnabled)) 'Expected false when CameraFile is null'

    # Missing file returns false by default.
    $script:CameraFile = Join-Path $tempRoot 'data/system/camera.properties'
    Assert-CameraCheck (-not (Get-CameraImagePickerEnabled)) 'Expected false when camera.properties is missing'

    # Enabling creates parent directory and writes image_picker_fallback=true.
    Set-CameraImagePickerEnabled $true
    Assert-CameraCheck (Test-Path -LiteralPath $script:CameraFile) 'Expected camera.properties to be created'
    Assert-CameraCheck (Get-CameraImagePickerEnabled) 'Expected true after Set-CameraImagePickerEnabled $true'

    # Unrelated properties/comments are preserved across enable/disable toggles.
    [IO.File]::WriteAllText($script:CameraFile, "#Custom header`nother_key=keep_me`nimage_picker_fallback=true`n", [Text.UTF8Encoding]::new($false))
    Set-CameraImagePickerEnabled $false
    Assert-CameraCheck (-not (Get-CameraImagePickerEnabled)) 'Expected false after Set-CameraImagePickerEnabled $false'
    $content = [IO.File]::ReadAllText($script:CameraFile, [Text.UTF8Encoding]::new($false))
    Assert-CameraCheck ($content -match 'other_key=keep_me') 'Expected unrelated property to be preserved'
    Assert-CameraCheck ($content -notmatch 'image_picker_fallback') 'Expected image_picker_fallback to be removed when disabled'

    # Case-insensitive and colon-separated values are parsed properly.
    [IO.File]::WriteAllText($script:CameraFile, "image_picker_fallback : TRUE`n", [Text.UTF8Encoding]::new($false))
    Assert-CameraCheck (Get-CameraImagePickerEnabled) 'Expected true for colon-separated uppercase TRUE'
    [IO.File]::WriteAllText($script:CameraFile, "image_picker_fallback=false`n", [Text.UTF8Encoding]::new($false))
    Assert-CameraCheck (-not (Get-CameraImagePickerEnabled)) 'Expected false for image_picker_fallback=false'

    Write-Host "PASS: Control panel camera image-picker setting ($checks checks)"
} finally {
    $resolvedTemp = [IO.Path]::GetFullPath($tempRoot)
    $tempPrefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if ($resolvedTemp.StartsWith($tempPrefix, [StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $resolvedTemp)) {
        Remove-Item -LiteralPath $resolvedTemp -Recurse -Force
    }
}
