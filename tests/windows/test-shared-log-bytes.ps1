# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
# Real-file regression for media tests reading an application-owned log.
# Keep this source ASCII for Windows PowerShell 5.1; build Unicode from code points.
param([string] $HelperPath = (Join-Path $PSScriptRoot 'helpers/SharedLogBytes.ps1'))
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2
. $HelperPath

$script:checks = 0
function Assert-SharedLog([bool] $Condition, [string] $Message) {
    if (-not $Condition) { throw "FAIL: $Message" }
    $script:checks++
}
function Assert-SharedLogIOException([scriptblock] $Action, [string] $Message) {
    $errorFound = $null
    try { $null = & $Action } catch { $errorFound = $_.Exception.GetBaseException() }
    Assert-SharedLog ($errorFound -is [IO.IOException]) $Message
}

$windows = [Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT
$work = Join-Path ([IO.Path]::GetTempPath()) ('gesher-shared-log-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
try {
    $log = Join-Path $work 'application.log'
    $prefixText = 'old ' + [char]0x05e9 + [char]0x05dc + [char]0x05d5 + [char]0x05dd + "`n"
    $suffixText = 'persistent input: ' + [char]0x4e2d + [char]0x6587 + ' PASS' + "`n"
    $prefix = [Text.Encoding]::UTF8.GetBytes($prefixText)
    $suffix = [Text.Encoding]::UTF8.GetBytes($suffixText)
    [IO.File]::WriteAllBytes($log, $prefix)
    $writer = [IO.FileStream]::new($log, [IO.FileMode]::Append, [IO.FileAccess]::Write,
        [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
    try {
        $writer.Write($suffix, 0, $suffix.Length)
        $writer.Flush()
        if ($windows) {
            Assert-SharedLogIOException { [IO.File]::ReadAllBytes($log) } `
                'The old ReadAllBytes reader rejects the live Windows writer'
        } else {
            Write-Host 'SKIP: Windows sharing-violation negative control (non-Windows host)'
        }

        $actual = Read-SharedLogBytes $log
        Assert-SharedLog ($actual -is [byte[]]) 'The result is one byte array, not enumerated bytes'
        Assert-SharedLog ($actual.Length -eq $prefix.Length + $suffix.Length) 'The snapshot has the complete current byte length'
        Assert-SharedLog ([Text.Encoding]::UTF8.GetString($actual) -ceq $prefixText + $suffixText) `
            'The open writer does not prevent exact UTF-8 log capture'
        Assert-SharedLog ($prefix.Length -gt $prefixText.Length) 'The prefix exercises a byte offset distinct from its character count'
        $tail = [Text.Encoding]::UTF8.GetString($actual, $prefix.Length, $actual.Length - $prefix.Length)
        Assert-SharedLog ($tail -ceq $suffixText) 'The existing byte-offset tail decode preserves multibyte text'
        $pipeline = @(Read-SharedLogBytes $log)
        Assert-SharedLog ($pipeline.Count -eq 1 -and $pipeline[0] -is [byte[]]) 'The pipeline receives a single array object'

        $more = [Text.Encoding]::UTF8.GetBytes('still writable')
        $writer.Write($more, 0, $more.Length)
        $writer.Flush()
        Assert-SharedLog ($actual.Length -eq $prefix.Length + $suffix.Length) 'Later appends do not extend the earlier snapshot'
        $next = Read-SharedLogBytes $log
        Assert-SharedLog ($next.Length -eq $actual.Length + $more.Length) 'The writer remains usable and a later read observes its append'
    } finally {
        $writer.Dispose()
    }

    # A returned reader must release its handle, so a later exclusive owner can open.
    $exclusive = [IO.FileStream]::new($log, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    try {
        Assert-SharedLog $exclusive.CanWrite 'The completed shared reader released its file handle'
        if ($windows) {
            Assert-SharedLogIOException { Read-SharedLogBytes $log } 'A genuinely incompatible Windows owner still raises an I/O error'
        } else {
            Write-Host 'SKIP: Windows exclusive-owner error control (non-Windows host)'
        }
    } finally {
        $exclusive.Dispose()
    }

    $empty = Join-Path $work 'empty.log'
    [IO.File]::WriteAllBytes($empty, [byte[]]::new(0))
    $emptyBytes = Read-SharedLogBytes $empty
    Assert-SharedLog ($emptyBytes -is [byte[]] -and $emptyBytes.Length -eq 0) 'An empty file returns an empty byte array, not null'
    $emptyPipeline = @(Read-SharedLogBytes $empty)
    Assert-SharedLog ($emptyPipeline.Count -eq 1 -and $emptyPipeline[0] -is [byte[]]) 'An empty byte array remains one pipeline value'

    $binary = Join-Path $work 'binary.log'
    $binaryBytes = [byte[]](0, 1, 128, 255)
    [IO.File]::WriteAllBytes($binary, $binaryBytes)
    $binaryResult = Read-SharedLogBytes $binary
    Assert-SharedLog ([Convert]::ToBase64String($binaryResult) -ceq [Convert]::ToBase64String($binaryBytes)) `
        'The helper reads bytes without decoding or modifying them'
    Assert-SharedLogIOException { Read-SharedLogBytes (Join-Path $work 'missing.log') } `
        'A missing file remains an I/O failure rather than an empty successful read'
    Assert-SharedLog (-not (Test-Path -LiteralPath (Join-Path $work 'missing.log'))) 'Reading a missing log never creates it'
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force
}
Write-Host "shared log bytes: $script:checks checks passed (PowerShell $($PSVersionTable.PSVersion))"
