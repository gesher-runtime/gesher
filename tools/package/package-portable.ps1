# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
<#
.SYNOPSIS
    Packages the installer's complete runtime as a portable ZIP.
.DESCRIPTION
    Uses the same [Files] manifest as the installer, including required resources
    and license notices. Missing inputs fail before a release archive is replaced.
    The archive contains GesherPortable/Gesher.exe, App/, Data/ and portable.marker.
.PARAMETER Version
    The exact installer version. If omitted, reads BuildDir/gesher-version.ini.
#>
[CmdletBinding()]
param(
    [string] $BuildDir = '',
    [string] $OutDir = 'installer',
    [string] $Version = '',
    [switch] $SkipZip
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$packageDir = $PSScriptRoot
$repoRoot = [IO.Path]::GetFullPath((Join-Path $packageDir '../..'))

if ([string]::IsNullOrWhiteSpace($BuildDir)) { $BuildDir = Join-Path $repoRoot 'dist' }
$BuildDir = [IO.Path]::GetFullPath($BuildDir)
if (-not (Test-Path -LiteralPath $BuildDir -PathType Container)) { throw "Runtime build directory does not exist: $BuildDir" }
if ([string]::IsNullOrWhiteSpace($Version)) {
    $metadata = Join-Path $BuildDir 'gesher-version.ini'
    if (Test-Path -LiteralPath $metadata -PathType Leaf) {
        $match = [regex]::Match([IO.File]::ReadAllText($metadata), '(?m)^Version=([0-9.]+)\r?$')
        if ($match.Success) { $Version = $match.Groups[1].Value }
    }
}
if ($Version -cnotmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(\.[1-9][0-9]*)?$' -or
    @($Version.Split('.') | Where-Object { $_.Length -gt 5 -or [int]$_ -gt 65535 }).Count -ne 0) {
    throw 'Pass the exact installer version with -Version (X.Y.Z or X.Y.Z.N).'
}
if (-not [IO.Path]::IsPathRooted($OutDir)) { $OutDir = Join-Path $repoRoot $OutDir }
$OutDir = [IO.Path]::GetFullPath($OutDir)
$null = [IO.Directory]::CreateDirectory($OutDir)
$finalStage = Join-Path $OutDir 'GesherPortable'
if ($SkipZip -and (Test-Path -LiteralPath $finalStage)) {
    throw "Portable output already exists; choose an empty output directory to preserve its Data: $finalStage"
}

# Resolve the trusted installer manifest before creating a staging directory.
# A new manifest directive must be handled explicitly instead of silently
# producing a different runtime. All paths are literal except its leaf globs.
$files = [Collections.Generic.List[object]]::new()
$destinations = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$inFiles = $false
foreach ($line in [IO.File]::ReadAllLines((Join-Path $packageDir 'gesher.iss'))) {
    if ($line -match '^\[([^\]]+)\]') { $inFiles = $Matches[1] -ceq 'Files'; continue }
    if (-not $inFiles -or [string]::IsNullOrWhiteSpace($line) -or $line.TrimStart().StartsWith(';')) { continue }
    $fields = @{}
    foreach ($part in $line.Split(';')) {
        if ($part -notmatch '^\s*([A-Za-z]+):\s*(.*?)\s*$') { throw "Unsupported installer file entry: $line" }
        $key = $Matches[1]; $value = $Matches[2]
        if ($fields.ContainsKey($key) -or $key -notin @('Source', 'DestDir', 'DestName', 'Flags', 'Languages')) {
            throw "Unsupported or duplicate installer file field: $key"
        }
        if ($value.StartsWith('"') -and $value.EndsWith('"')) { $value = $value.Substring(1, $value.Length - 2) }
        $fields[$key] = $value
    }
    if ($fields.ContainsKey('Languages') -and $fields.Languages -cne 'en') { continue }
    if (-not $fields.ContainsKey('Source') -or -not $fields.ContainsKey('DestDir') -or
        $fields.DestDir -notmatch '^\{app\}(\\[^{}]*)?$') { throw "Unsupported installer file destination: $line" }
    $destination = $fields.DestDir.Substring(5).TrimStart('\').Replace('\', '/')
    if ($destination.Split('/') -contains '..') { throw 'Installer file destination escapes the runtime.' }
    $flags = @()
    if ($fields.ContainsKey('Flags')) { $flags = @($fields.Flags.Split(' ') | Where-Object { $_ }) }
    if (@($flags | Where-Object { $_ -notin @('ignoreversion', 'recursesubdirs') }).Count) { throw "Unsupported installer file flags: $($fields.Flags)" }
    $sourceRoot = $packageDir
    $relative = $fields.Source
    if ($relative.StartsWith('{#BuildDir}\', [StringComparison]::Ordinal)) {
        $sourceRoot = $BuildDir; $relative = $relative.Substring(12)
    }
    $relative = $relative.Replace('\', '/')
    if ($relative.Contains('{') -or [IO.Path]::IsPathRooted($relative) -or $relative.Split('/') -contains '..') { throw "Unsupported installer file source: $($fields.Source)" }
    $parent = [IO.Path]::GetDirectoryName($relative)
    $sourceDirectory = if ($parent) { Join-Path $sourceRoot $parent } else { $sourceRoot }
    $pattern = [IO.Path]::GetFileName($relative)
    $recursive = $flags -contains 'recursesubdirs'
    if (-not (Test-Path -LiteralPath $sourceDirectory -PathType Container)) { throw "Missing runtime directory: $sourceDirectory" }
    $sourceFiles = @(Get-ChildItem -LiteralPath $sourceDirectory -Filter $pattern -File -Recurse:$recursive)
    if ($sourceFiles.Count -eq 0) { throw "Missing required runtime input: $($fields.Source)" }
    foreach ($file in $sourceFiles) {
        if (($file.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw "Runtime input is a link: $($file.FullName)" }
        $name = if ($fields.ContainsKey('DestName')) { $fields.DestName } elseif ($recursive) {
            $file.FullName.Substring($sourceDirectory.Length).TrimStart('\', '/').Replace('\', '/')
        } else { $file.Name }
        $target = if ($destination) { "$destination/$name" } else { $name }
        if (-not $destinations.Add($target)) { throw "Duplicate runtime destination: $target" }
        $files.Add([PSCustomObject]@{Source = $file.FullName; Destination = $target})
    }
}
if (-not $destinations.Contains('gesher-launch.exe') -or -not $destinations.Contains('dalvikvm.exe') -or
    -not $destinations.Contains('gesher-panel-native.ps1')) { throw 'Installer runtime manifest is incomplete.' }

$scratchStage = Join-Path $OutDir ('.portable-stage-' + [Guid]::NewGuid().ToString('N'))
$staging = Join-Path $scratchStage 'GesherPortable'
$temporaryZip = Join-Path $OutDir ('.portable-' + [Guid]::NewGuid().ToString('N') + '.zip')
try {
    $app = Join-Path $staging 'App'
    $null = [IO.Directory]::CreateDirectory($app)
    $null = [IO.Directory]::CreateDirectory((Join-Path $staging 'Data'))
    [IO.File]::WriteAllText((Join-Path $staging 'portable.marker'), '')
    foreach ($file in $files) {
        $target = Join-Path $app $file.Destination
        $null = [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
        [IO.File]::Copy($file.Source, $target, $false)
    }
    # Keep the built launcher byte-identical in the portable root and App.
    # Its resources belong to the native build, not to a post-build PE editor.
    [IO.File]::Copy((Join-Path $app 'gesher-launch.exe'), (Join-Path $staging 'Gesher.exe'), $false)
    [IO.File]::WriteAllText((Join-Path $app 'gesher-version.ini'), "[Gesher]`r`nVersion=$Version`r`n", [Text.Encoding]::ASCII)
    [IO.File]::Copy((Join-Path $packageDir 'README-portable.txt'), (Join-Path $staging 'README.txt'), $false)
    foreach ($notice in 'security-notice.txt', 'security-notice.he.txt') { [IO.File]::Copy((Join-Path $packageDir $notice), (Join-Path $staging $notice), $false) }
    if ($SkipZip) {
        [IO.Directory]::Move($staging, $finalStage)
        Write-Host "Portable package staged at: $finalStage"
    } else {
        $zipName = "gesher-$Version-portable.zip"
        $zipPath = Join-Path $OutDir $zipName
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        [IO.Compression.ZipFile]::CreateFromDirectory($staging, $temporaryZip, [IO.Compression.CompressionLevel]::Optimal, $true)
        $hash = (Get-FileHash -LiteralPath $temporaryZip -Algorithm SHA256).Hash.ToLowerInvariant()
        if ([IO.File]::Exists($zipPath)) { [IO.File]::Replace($temporaryZip, $zipPath, $null) }
        else { [IO.File]::Move($temporaryZip, $zipPath) }
        $sum = "$hash  $zipName"
        [IO.File]::WriteAllText("$zipPath.sha256", "$sum`r`n", [Text.Encoding]::ASCII)
        $sumsPath = Join-Path $OutDir 'SHA256SUMS.txt'
        $lines = @()
        if ([IO.File]::Exists($sumsPath)) { $lines = @([IO.File]::ReadAllLines($sumsPath) | Where-Object { $_ -notmatch ('\s+' + [regex]::Escape($zipName) + '$') }) }
        [IO.File]::WriteAllLines($sumsPath, [string[]]($lines + $sum), [Text.Encoding]::ASCII)
        Write-Host "Portable package created: $zipPath (SHA256: $hash)"
    }
} finally {
    if ([IO.File]::Exists($temporaryZip)) { [IO.File]::Delete($temporaryZip) }
    if ([IO.Directory]::Exists($scratchStage)) { [IO.Directory]::Delete($scratchStage, $true) }
}
