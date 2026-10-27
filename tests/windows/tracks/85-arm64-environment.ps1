# ADR0067: ARM LP64 environment/default object, actual current guest processes.
$environmentMarker = 'native2 environment: ok; 256 pinned rows, eight-byte canaries, imported/dynamic default, errno, threads and nested callbacks'
foreach ($environmentMode in 'dalvikvm', 'single', 'device') {
  $environmentRecords = @($arm64FpcrEvidence | Where-Object { $_.Abi -eq 'arm64' -and $_.Mode -eq $environmentMode })
  if ($environmentRecords.Count -eq 1 -and $environmentRecords[0].Exit -eq 0 -and
      $environmentRecords[0].Report.Contains($environmentMarker)) {
    Write-Host "PASS  arm64 environment ($environmentMode): 256 pinned rows, default object, canaries, errno, threads and nested callbacks"
  } else {
    Write-Host "FAIL  arm64 environment ($environmentMode): actual process did not verify every environment contract"
    foreach ($record in $environmentRecords) { Write-Host $record.Report }
    $failed++
  }
}
Check 'arm64 environment: actual native ELF process verifies complete pinned environment and default object' {
  A64Run "$a64/gesher-exec.exe" @('--elf', "$a64/arm64-env-exec", 'arm64-env-exec')
} @('PASS arm64 environment guest: 256 pinned rows, eight-byte canaries, imported/dynamic default, errno, threads and nested callbacks')
