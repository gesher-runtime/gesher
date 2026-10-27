# Real public Files.copy and the exact pinned UnixCopyFile JNI entry.
$copyRoot = NoDrive (Join-Path $root ('libcore-copy-' + [guid]::NewGuid().ToString('N')))
$copyOutput = RunTimed "$Dir/dalvikvm.exe" @(
  "-Xbootclasspath:$bcp", "-Ximage:$(NoDrive "$root/system")/framework/boot.art", '-Xint', '-Xusejit:false',
  '-cp', (NoDrive "$Dir/libcore-tests.dex"), 'gesher.CopyFileTest', $copyRoot)
$copyExitCode = $LASTEXITCODE
# Preserve the actual child records in plain CI logs, including successful
# groups. The shared CheckTimed helper otherwise prints only its verdict.
Write-Host $copyOutput
$copyExpect = @(
  'PASS  libcore copy: Files.copy exact bytes 0',
  'PASS  libcore copy: Files.copy exact bytes 1',
  'PASS  libcore copy: Files.copy exact bytes 8191',
  'PASS  libcore copy: Files.copy exact bytes 8192',
  'PASS  libcore copy: Files.copy exact bytes 8193',
  'PASS  libcore copy: Files.copy exact bytes 2097223',
  'PASS  libcore copy: existing destination is preserved without replacement',
  'PASS  libcore copy: explicit replacement copies every byte',
  'PASS  libcore copy: missing source leaves no partial destination',
  'PASS  libcore copy: native transfer honors both offsets and retains caller descriptor ownership',
  'PASS  libcore copy: real cancellation is checked after read and before any write',
  'PASS  libcore copy: source EBADF precedes cancellation',
  'PASS  libcore copy: EOF precedes cancellation and destination validation',
  'PASS  libcore copy: destination EBADF follows the actual source read',
  'PASS  libcore copy: clearing the same cancellation word permits a complete real transfer',
  'CopyFileTest all passed')
$copyMissing = @($copyExpect | Where-Object { -not $copyOutput.Contains($_) })
if ($copyExitCode -eq 0 -and $copyMissing.Count -eq 0) {
  Write-Host 'PASS  libcore copy: public files, actual JNI offsets, errors and real cancellation'
} else {
  Write-Host "FAIL  libcore copy: public files, actual JNI offsets, errors and real cancellation (exit $copyExitCode; missing: $($copyMissing -join ', '))"
  $script:failed++
}
