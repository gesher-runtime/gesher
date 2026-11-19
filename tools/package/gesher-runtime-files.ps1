# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
# Synchronize only runtime-owned files into the Android profile. The installed
# version invalidates the cache once per upgrade, including equal-size files
# with identical timestamps. Ordinary launches do not hash large ICU/font files.

function New-GesherRuntimeSync {
    param(
        [Parameter(Mandatory = $true)][string] $InstallDirectory,
        [Parameter(Mandatory = $true)][string] $ProfileDirectory,
        [Parameter(Mandatory = $true)][ValidateSet('app', 'dex')][string] $Component
    )
    $metadata = Join-Path $InstallDirectory 'gesher-version.ini'
    $generation = $null
    if (Test-Path -LiteralPath $metadata -PathType Leaf) {
        $info = Get-Item -LiteralPath $metadata -Force
        if ($info.Length -le 4096) {
            $text = [IO.File]::ReadAllText($info.FullName)
            # Setup owns this small file. Accept its unambiguous canonical form (X.Y.Z for a release, X.Y.Z.N for a CI build);
            # corrupt/extended metadata safely falls back to checking content.
            $match = [regex]::Match($text,
                '\A\s*\[Gesher\]\s*\r?\nVersion=((0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})(\.[1-9][0-9]{0,4})?)\s*\z')
            if ($match.Success -and @($match.Groups[1].Value.Split('.') | Where-Object { [int]$_ -gt 65535 }).Count -eq 0) {
                $generation = [IO.Path]::GetFullPath($InstallDirectory) + "`n" + $match.Groups[1].Value
            }
        }
    }
    $stamp = Join-Path $ProfileDirectory ("system/.gesher-runtime-{0}.version" -f $Component)
    $previous = $null
    if ($generation -and (Test-Path -LiteralPath $stamp -PathType Leaf)) {
        try {
            if ((Get-Item -LiteralPath $stamp -Force).Length -le 8192) {
                $previous = [IO.File]::ReadAllText($stamp)
            }
        } catch { } # A missing/unreadable stamp requires a complete refresh.
    }
    return [PSCustomObject]@{
        Generation = $generation
        StampPath = $stamp
        ForceRefresh = ($null -ne $generation -and $generation -cne $previous)
    }
}

function Get-GesherRuntimeFileHash {
    param([Parameter(Mandatory = $true)][string] $Path)
    # Packaged launchers must not depend on Get-FileHash/module availability in
    # their PowerShell host. Stream large ICU/font resources and allow readers
    # that already have identical runtime bytes mapped to remain open.
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
        try {
            return [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-', '')
        } finally { $stream.Dispose() }
    } finally { $algorithm.Dispose() }
}

function Sync-GesherRuntimeFile {
    param(
        [Parameter(Mandatory = $true)][string] $Source,
        [Parameter(Mandatory = $true)][string] $Destination,
        [Parameter(Mandatory = $true)] $State
    )
    $sourceInfo = Get-Item -LiteralPath $Source -ErrorAction Stop
    # A missing literal path must not be acknowledged even if the filesystem
    # provider returns no item without a terminating error (Windows PowerShell).
    if ($null -eq $sourceInfo -or $sourceInfo.PSIsContainer) {
        throw "Expected an existing runtime file: $Source"
    }
    $copy = $false
    if (-not (Test-Path -LiteralPath $Destination -PathType Leaf)) {
        $copy = $true
    } else {
        $destinationInfo = Get-Item -LiteralPath $Destination -ErrorAction Stop
        if ($sourceInfo.Length -ne $destinationInfo.Length) {
            $copy = $true
        } elseif ($State.ForceRefresh -or -not $State.Generation -or
                  $sourceInfo.LastWriteTimeUtc -ne $destinationInfo.LastWriteTimeUtc) {
            # The other launcher may already have copied and mapped this file.
            # A new component stamp requires checking it, not rewriting identical
            # bytes that a running ART process can have open without write sharing.
            # Unversioned builds also cannot use metadata as a release identity.
            $copy = (Get-GesherRuntimeFileHash -Path $sourceInfo.FullName) -cne
                    (Get-GesherRuntimeFileHash -Path $destinationInfo.FullName)
            if (-not $copy -and $sourceInfo.LastWriteTimeUtc -ne $destinationInfo.LastWriteTimeUtc) {
                # Avoid repeating a timestamp-only comparison on later launches.
                # An existing reader may deny attribute writes; its bytes are
                # already correct, so that must not prevent an app from starting.
                try { [IO.File]::SetLastWriteTimeUtc($destinationInfo.FullName, $sourceInfo.LastWriteTimeUtc) } catch { }
            }
        }
    }
    if ($copy) {
        Copy-Item -LiteralPath $Source -Destination $Destination -ErrorAction Stop
    }
}

function Complete-GesherRuntimeSync {
    param([Parameter(Mandatory = $true)] $State)
    if (-not $State.Generation -or -not $State.ForceRefresh) { return }
    # Called only after every requested copy succeeds. Separate app/dex stamps
    # keep the small dex launcher from acknowledging the full framework refresh.
    $parent = Split-Path -Parent $State.StampPath
    $null = [IO.Directory]::CreateDirectory($parent)
    $temporary = Join-Path $parent ([IO.Path]::GetRandomFileName())
    try {
        [IO.File]::WriteAllText($temporary, $State.Generation, [Text.UTF8Encoding]::new($false))
        if ([IO.File]::Exists($State.StampPath)) {
            [IO.File]::Replace($temporary, $State.StampPath, [NullString]::Value)
        } else {
            try { [IO.File]::Move($temporary, $State.StampPath) } catch [IO.IOException] {
                if (-not [IO.File]::Exists($State.StampPath)) { throw }
                # Another launcher may have completed the same generation first.
                [IO.File]::Replace($temporary, $State.StampPath, [NullString]::Value)
            }
        }
    } finally {
        if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) }
    }
}
