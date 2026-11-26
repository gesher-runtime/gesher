# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
# Read one byte-length snapshot while the application's log writer stays open.
# Unlike ReadAllBytes, this reader permits existing writers and log rotation.
function Read-SharedLogBytes([string] $Path) {
    $ErrorActionPreference = 'Stop'
    $stream = [IO.FileStream]::new($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
    try {
        $length = $stream.Length
        if ($length -gt [int]::MaxValue) {
            throw [IO.IOException]::new('The log snapshot is too large for a byte array.')
        }
        $bytes = [byte[]]::new([int]$length)
        $offset = 0
        while ($offset -lt $bytes.Length) {
            $count = $stream.Read($bytes, $offset, $bytes.Length - $offset)
            if ($count -eq 0) {
                throw [IO.EndOfStreamException]::new('The log was truncated while reading its byte snapshot.')
            }
            $offset += $count
        }
        # Do not enumerate bytes into the pipeline, including an empty array.
        return ,$bytes
    } finally {
        $stream.Dispose()
    }
}
