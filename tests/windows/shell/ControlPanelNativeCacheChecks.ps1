# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
# Execute the production helper loader in fresh processes. No user's cache is touched.
param(
    [string] $NativePath = (Join-Path $PSScriptRoot '../../../tools/package/gesher-panel-native.ps1'),
    [switch] $LoadOnly,
    [string] $CacheRoot,
    [string] $ResultFile
)
$ErrorActionPreference = 'Stop'
if ($LoadOnly) {
    $env:LOCALAPPDATA = $CacheRoot
    $clock = [Diagnostics.Stopwatch]::StartNew()
    . $NativePath
    $clock.Stop()
    $monitor = 'Gesher.PanelChangeMonitor' -as [type]
    $shellGroup = 'Gesher.Win32ShellGroup' -as [type]
    if (-not $monitor -or -not $shellGroup -or $monitor::MaximumWatchers -ne 8 -or
            -not $monitor.GetMethod('SetFiles') -or -not $monitor.GetMethod('Dispose')) {
        throw 'Production native helper did not load its complete contract'
    }
    @{ Location = $monitor.Assembly.Location; LoadMs = $clock.ElapsedMilliseconds } |
        ConvertTo-Json -Compress | Set-Content -LiteralPath $ResultFile -Encoding UTF8
    return
}

$script:Checks = 0
function Assert-Cache([bool] $okay, [string] $reason) {
    if (-not $okay) { throw $reason }
    $script:Checks++
}
$nativeFile = [IO.Path]::GetFullPath($NativePath)
$testFile = [IO.Path]::GetFullPath($PSCommandPath)
$shellName = if ($PSVersionTable.PSEdition -eq 'Core') {
    if ($env:OS -eq 'Windows_NT') { 'pwsh.exe' } else { 'pwsh' }
} else { 'powershell.exe' }
$shell = Join-Path $PSHOME $shellName
function Quote-CacheLiteral([string] $value) { return "'" + $value.Replace("'", "''") + "'" }
function Start-CacheLoad([string] $cache, [string] $result, [string] $source = $nativeFile) {
    $code = '& ' + (Quote-CacheLiteral $testFile) + ' -LoadOnly -NativePath ' + (Quote-CacheLiteral $source) +
        ' -CacheRoot ' + (Quote-CacheLiteral $cache) + ' -ResultFile ' + (Quote-CacheLiteral $result)
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $shell
    $info.Arguments = '-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand ' +
        [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($code))
    $info.UseShellExecute = $false; $info.CreateNoWindow = $true
    return [Diagnostics.Process]::Start($info)
}
function Finish-CacheLoad($process, [string] $result) {
    try {
        if (-not $process.WaitForExit(30000)) { $process.Kill(); throw 'Native helper child timed out' }
        if ($process.ExitCode -ne 0) { throw "Native helper child failed: $($process.ExitCode)" }
        return (Get-Content -LiteralPath $result -Raw -Encoding UTF8 | ConvertFrom-Json)
    } finally { $process.Dispose() }
}
function Load-Cache([string] $cache, [string] $result, [string] $source = $nativeFile) {
    Finish-CacheLoad (Start-CacheLoad $cache $result $source) $result
}

$work = Join-Path ([IO.Path]::GetTempPath()) ('gesher-native-cache-' + [guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($work)
$left = $null; $right = $null
try {
    $cache = Join-Path $work 'profile'
    $cold = Load-Cache $cache (Join-Path $work 'cold.json')
    Assert-Cache (-not [string]::IsNullOrEmpty($cold.Location) -and [IO.File]::Exists($cold.Location)) 'Cold loader did not publish a reusable assembly'
    Assert-Cache ($cold.Location.StartsWith($cache, [StringComparison]::OrdinalIgnoreCase)) 'Loader used a cache outside the fixture'
    $stamp = [IO.File]::GetLastWriteTimeUtc($cold.Location)
    $hash = (Get-FileHash -LiteralPath $cold.Location -Algorithm SHA256).Hash
    $warm = Load-Cache $cache (Join-Path $work 'warm.json')
    Assert-Cache ($warm.Location -ceq $cold.Location) 'Warm loader did not reuse the compiled assembly'
    Assert-Cache ([IO.File]::GetLastWriteTimeUtc($cold.Location) -eq $stamp -and
        (Get-FileHash -LiteralPath $cold.Location -Algorithm SHA256).Hash -ceq $hash) 'Warm loader rewrote the unchanged assembly'

    [IO.File]::WriteAllText($cold.Location, 'incomplete cached assembly')
    $repaired = Load-Cache $cache (Join-Path $work 'repaired.json')
    Assert-Cache ($repaired.Location -ceq $cold.Location -and (Get-Item -LiteralPath $cold.Location).Length -gt 1024) 'Corrupt cache did not recover'

    $otherSource = Join-Path $work 'different-native.ps1'
    $sourceText = [IO.File]::ReadAllText($nativeFile).Replace('using System;', 'using System; // distinct source input')
    [IO.File]::WriteAllText($otherSource, $sourceText)
    $changed = Load-Cache $cache (Join-Path $work 'changed.json') $otherSource
    Assert-Cache ($changed.Location -cne $cold.Location -and [IO.File]::Exists($cold.Location)) 'Different source reused or overwrote the earlier cache entry'

    $concurrent = Join-Path $work 'concurrent'
    $leftFile = Join-Path $work 'left.json'; $rightFile = Join-Path $work 'right.json'
    $left = Start-CacheLoad $concurrent $leftFile
    $right = Start-CacheLoad $concurrent $rightFile
    $leftResult = Finish-CacheLoad $left $leftFile; $left = $null
    $rightResult = Finish-CacheLoad $right $rightFile; $right = $null
    # A racing child may safely use the documented in-memory fallback. Both children
    # checked the complete helper contract; any disk loads must use one published entry.
    $diskLocations = @(@($leftResult.Location, $rightResult.Location) |
        Where-Object { -not [string]::IsNullOrEmpty($_) } | Select-Object -Unique)
    $publishedFiles = @(Get-ChildItem -LiteralPath $concurrent -Recurse -Filter 'Gesher.PanelNative.*.dll' |
        Where-Object { $_.Name -notlike '*.tmp.dll' })
    Assert-Cache ($publishedFiles.Count -eq 1) 'Concurrent loaders did not publish exactly one cache entry'
    $published = Load-Cache $concurrent (Join-Path $work 'published.json')
    Assert-Cache ($published.Location -ceq $publishedFiles[0].FullName -and
        @($diskLocations | Where-Object { $_ -cne $published.Location }).Count -eq 0) 'Concurrent loaders did not leave a reusable cache entry'
    Assert-Cache (@(Get-ChildItem -LiteralPath $concurrent -Recurse -Filter '*.tmp.dll').Count -eq 0) 'Concurrent loader retained its temporary assembly'

    $blocked = Join-Path $work 'blocked'
    [void][IO.Directory]::CreateDirectory((Join-Path $blocked 'Gesher'))
    [IO.File]::WriteAllText((Join-Path $blocked 'Gesher/cache'), 'not a directory')
    $fallback = Load-Cache $blocked (Join-Path $work 'fallback.json')
    Assert-Cache ([string]::IsNullOrEmpty($fallback.Location)) 'Unavailable cache prevented the in-memory fallback'
    Write-Host "PASS: native helper cache ($script:Checks checks; fresh-process cold=$($cold.LoadMs)ms, warm=$($warm.LoadMs)ms; component timings only)"
} finally {
    foreach ($process in @($left, $right)) {
        if ($null -ne $process) {
            try { if (-not $process.HasExited) { $process.Kill(); [void]$process.WaitForExit(5000) } } finally { $process.Dispose() }
        }
    }
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
