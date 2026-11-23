# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
# Synchronize only runtime-owned files into the Android profile. The installed
# version invalidates the cache once per upgrade, including equal-size files
# with identical timestamps. Ordinary launches do not read large ICU/font files.

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

function Get-GesherRuntimeByteComparer {
    # Resolve the platform's byte comparator once per launcher. Reflection.Emit
    # works in both Windows PowerShell 5.1 and PowerShell 7 without Add-Type's
    # external compiler or a module import. Buffers remain pinned by the P/Invoke
    # marshaler for the call; callers bound count by both arrays' valid data.
    if ($null -eq $script:GesherRuntimeByteComparer) {
        $name = [Reflection.AssemblyName]::new('Gesher.RuntimeByteComparer')
        $access = [Reflection.Emit.AssemblyBuilderAccess]::Run
        if ($PSVersionTable.PSEdition -eq 'Core') {
            $assembly = [Reflection.Emit.AssemblyBuilder]::DefineDynamicAssembly($name, $access)
        } else {
            $assembly = [AppDomain]::CurrentDomain.DefineDynamicAssembly($name, $access)
        }
        $module = $assembly.DefineDynamicModule('Comparer')
        $type = $module.DefineType('Gesher.RuntimeByteComparer', [Reflection.TypeAttributes]::Public)
        $library = if ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT) { 'msvcrt.dll' } else { 'libc' }
        $method = $type.DefinePInvokeMethod('memcmp', $library,
            [Reflection.MethodAttributes]::Public -bor [Reflection.MethodAttributes]::Static,
            [Reflection.CallingConventions]::Standard, [int], [Type[]]@([byte[]], [byte[]], [UIntPtr]),
            [Runtime.InteropServices.CallingConvention]::Cdecl, [Runtime.InteropServices.CharSet]::Ansi)
        $method.SetImplementationFlags($method.GetMethodImplementationFlags() -bor [Reflection.MethodImplAttributes]::PreserveSig)
        # Keep the created type itself: Windows PowerShell 5.1 cannot parse a
        # generic delegate type literal with array arguments inside a call.
        $script:GesherRuntimeByteComparer = $type.CreateType()
    }
    return $script:GesherRuntimeByteComparer
}

function Read-GesherRuntimeChunk {
    param([IO.Stream] $Stream, [byte[]] $Buffer)
    $count = 0
    while ($count -lt $Buffer.Length) {
        $read = $Stream.Read($Buffer, $count, $Buffer.Length - $count)
        if ($read -eq 0) { break }
        $count += $read
    }
    return $count
}

function Test-GesherRuntimeFileEqual {
    param(
        [Parameter(Mandatory = $true)][string] $Source,
        [Parameter(Mandatory = $true)][string] $Destination
    )
    # Unversioned builds still compare every byte, even with equal length and
    # timestamps. An exact streaming comparison avoids two SHA-256 computations
    # and can stop at the first different chunk. Both handles deny writes on
    # Windows while comparing, and retain read sharing for mapped runtime files.
    $left = [IO.File]::Open($Source, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        $right = [IO.File]::Open($Destination, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
        try {
            if ($left.Length -ne $right.Length) { return $false }
            if ($left.Length -eq 0) { return $true }
            $compare = Get-GesherRuntimeByteComparer
            # Bound memory to 512 KiB while keeping PowerShell dispatch out of
            # the per-small-read path for ICU/font resources. Small resources
            # allocate only their actual size.
            $bufferLength = [int][Math]::Min($left.Length, 262144)
            $leftBuffer = [byte[]]::new($bufferLength)
            $rightBuffer = [byte[]]::new($bufferLength)
            do {
                $leftCount = Read-GesherRuntimeChunk $left $leftBuffer
                $rightCount = Read-GesherRuntimeChunk $right $rightBuffer
                if ($leftCount -ne $rightCount) { return $false }
                if ($leftCount -eq 0) { return $true }
                if ($compare::memcmp($leftBuffer, $rightBuffer, [UIntPtr]::new([uint64] $leftCount)) -ne 0) { return $false }
            } while ($true)
        } finally { $right.Dispose() }
    } finally { $left.Dispose() }
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
            $copy = -not (Test-GesherRuntimeFileEqual -Source $sourceInfo.FullName -Destination $destinationInfo.FullName)
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

function Resolve-GesherAsciiPath {
    param([Parameter(Mandatory = $false)][AllowEmptyString()][string] $Path)
    if ([string]::IsNullOrEmpty($Path) -or $Path -notmatch '[^\x00-\x7F]') {
        return $Path
    }
    $native = $Path.Replace('/', '\')
    if ($native.StartsWith('\') -and -not $native.StartsWith('\\')) {
        $native = (Get-Location).Drive.Name + ':' + $native
    } elseif ($native -notmatch '^[A-Za-z]:\\') {
        return $Path
    }
    try {
        $native = [IO.Path]::GetFullPath($native)
        $hadTrailingSep = ($native.Length -gt 3 -and $native.EndsWith('\'))
        $probe = if ($native.Length -gt 3) { $native.TrimEnd('\') } else { $native }
        $trailing = [System.Collections.Generic.List[string]]::new()
        # The leaf keeps its long name: an 8.3 alias truncates and upper-cases
        # extensions (.xapk -> .XAP), and callers key behaviour on file names.
        # Only the ancestors are shortened; a non-ASCII leaf leaves the whole
        # path long (see the check at the end).
        $leafName = [IO.Path]::GetFileName($probe)
        $leafParent = [IO.Path]::GetDirectoryName($probe)
        if ($leafName -and $leafParent -and $leafParent -ne $probe) {
            $trailing.Add($leafName)
            $probe = if ($leafParent.Length -gt 3) { $leafParent.TrimEnd('\') } else { $leafParent }
        }
        while ($probe -and (([IO.Path]::GetFileName($probe) -notmatch '[^\x00-\x7F]') -or -not ([IO.Directory]::Exists($probe) -or [IO.File]::Exists($probe)))) {
            $leaf = [IO.Path]::GetFileName($probe)
            $parent = [IO.Path]::GetDirectoryName($probe)
            if (-not $parent -or $parent -eq $probe) { break }
            $trailing.Add($leaf)
            $probe = if ($parent.Length -gt 3) { $parent.TrimEnd('\') } else { $parent }
        }
        if (-not ([IO.Directory]::Exists($probe) -or [IO.File]::Exists($probe))) {
            return $Path
        }
        if ($null -eq $script:GesherFso) {
            $script:GesherFso = [Activator]::CreateInstance([Type]::GetTypeFromProgID('Scripting.FileSystemObject'))
        }
        $short = if ([IO.Directory]::Exists($probe)) {
            $script:GesherFso.GetFolder($probe).ShortPath
        } else {
            $script:GesherFso.GetFile($probe).ShortPath
        }
        if ([string]::IsNullOrEmpty($short)) {
            return $Path
        }
        for ($i = $trailing.Count - 1; $i -ge 0; $i--) {
            $short = [IO.Path]::Combine($short, $trailing[$i])
        }
        if ($hadTrailingSep -and -not $short.EndsWith('\')) {
            $short += '\'
        }
        if ($short -notmatch '[^\x00-\x7F]') {
            return $short
        }
    } catch {
        # Volumes with 8.3 short names disabled or restricted COM environments fall back cleanly.
    }
    return $Path
}

function ConvertTo-GesherAndroidPath {
    param(
        [Parameter(Mandatory = $false)][AllowEmptyString()][string] $Path,
        [Parameter(Mandatory = $false)][AllowEmptyString()][string] $BaseDrive = '',
        # User-supplied paths (an APK, a dex, other arguments) are passed as
        # given: only Gesher's own roots are rewritten to 8.3 short paths.
        [switch] $KeepLongPath
    )
    if ([string]::IsNullOrEmpty($Path)) { return $Path }
    $resolved = if ($KeepLongPath) { $Path } else { Resolve-GesherAsciiPath -Path $Path }
    $posix = $resolved.Replace('\', '/')
    if ($posix -match '^([A-Za-z]):(.*)$') {
        $drive = $Matches[1]
        $rest = $Matches[2]
        if ([string]::IsNullOrEmpty($BaseDrive)) {
            $BaseDrive = (Get-Location).Drive.Name
        }
        if ($drive -ieq $BaseDrive) { return $rest }
        return "/mnt/$($drive.ToLower())$rest"
    }
    return $posix
}
