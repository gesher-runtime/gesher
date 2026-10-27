# ADR 0067's first slice: actual translated FPCR and native host boundaries.
Check 'arm64-2: real FPCR rounding, nested callbacks, thread isolation and restored native host fenv' {
  A64Run "$a64/arm64_fpcr_test.exe" @("$a64/arm64-fpcr-code.bin")
} @('PASS  arm64 FPCR: four modes, real positive/negative arithmetic and preserved FPCR bits',
  'PASS  arm64 FPCR: invalid mode rejection preserves actual state',
  'PASS  arm64 FPCR: direct MSR and nested guest-host callbacks inherit/persist state and restore host fenv',
  'PASS  arm64 FPCR: concurrent guest thread isolation and native host fenv restoration',
  '0 test(s) failed')

foreach ($fpcrMode in 'dalvikvm', 'single', 'device') {
  $fpcrRecords = @($arm64FpcrEvidence | Where-Object { $_.Abi -eq 'arm64' -and $_.Mode -eq $fpcrMode })
  if ($fpcrRecords.Count -eq 1 -and $fpcrRecords[0].Exit -eq 0 -and
      $fpcrRecords[0].Report.Contains('native2 fpcr: ok; pinned FPCR modes0..3, actual arithmetic, bits, pthread inheritance/isolation and nested callbacks')) {
    Write-Host "PASS  arm64-2 FPCR ($fpcrMode): pinned modes, actual arithmetic, bits, pthread inheritance/isolation and nested callbacks"
  } else {
    Write-Host "FAIL  arm64-2 FPCR ($fpcrMode): actual NDK process did not verify every rounding contract"
    foreach ($fpcrRecord in $fpcrRecords) { Write-Host $fpcrRecord.Report }
    $failed++
  }
}
Check 'arm64-2 FPCR: an actual native ELF process verifies pinned rounding, threads and nested callbacks' {
  A64Run "$a64/gesher-exec.exe" @('--elf', "$a64/arm64-fpcr-exec", 'arm64-fpcr-exec')
} @('PASS arm64 FPCR guest: pinned modes0..3, actual arithmetic, bits, pthread inheritance/isolation and nested callbacks')
