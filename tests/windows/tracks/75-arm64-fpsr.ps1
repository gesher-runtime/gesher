# ADR0067's sticky-FPSR slice. All artifacts and execution are CI-only.
Check 'arm64-2 FPSR: real flags, arithmetic, nested callbacks, thread isolation and restored native fenv' {
  A64Run "$a64/arm64_fpsr_test.exe" @("$a64/arm64-fpcr-code.bin", "$a64/arm64-fpsr-reference.txt")
} @('PASS  arm64 FPSR: real status, pinned masks, denormal, four-byte flags and preserved QC',
  'PASS  arm64 FPSR: real FDIV FMUL FADD and saturating SIMD produce all six flags and QC',
  'PASS  arm64 FPSR: direct MSR and three guest levels inherit/persist status and restore native fenv',
  'PASS  arm64 FPSR: concurrent guest state is isolated and every native host fenv is restored',
  'PASS  arm64 FPSR: 8192 independent QEMU numeric/status cases, scalar32/64 add sub mul div, all modes DN/FZ and NaN/inf/subnormal rules',
  'PASS  arm64 FPSR: 144 independent QEMU cumulative cases preserve seeded and same-block IXC before and after FZ underflow',
  '0 test(s) failed')

# Reuse complete actual process logs already captured by test44 for test69.
foreach ($fpsrMode in 'dalvikvm', 'single', 'device') {
  $fpsrRecords = @($arm64FpcrEvidence | Where-Object { $_.Abi -eq 'arm64' -and $_.Mode -eq $fpsrMode })
  if ($fpsrRecords.Count -eq 1 -and $fpsrRecords[0].Exit -eq 0 -and
      $fpsrRecords[0].Report.Contains('native2 fpsr: ok; pinned FPSR flags0x9f, masks, real arithmetic, QC, pthread inheritance/isolation and nested callbacks')) {
    Write-Host "PASS  arm64-2 FPSR ($fpsrMode): pinned flags, masks, real arithmetic, QC, pthread inheritance/isolation and nested callbacks"
  } else {
    Write-Host "FAIL  arm64-2 FPSR ($fpsrMode): actual NDK process did not verify every sticky-status contract"
    foreach ($fpsrRecord in $fpsrRecords) { Write-Host $fpsrRecord.Report }
    $failed++
  }
}
Check 'arm64-2 FPSR: an actual native ELF process verifies pinned status, arithmetic, threads and nested callbacks' {
  A64Run "$a64/gesher-exec.exe" @('--elf', "$a64/arm64-fpsr-exec", 'arm64-fpsr-exec')
} @('PASS arm64 FPSR guest: pinned flags0x9f, masks, real arithmetic, QC, pthread inheritance/isolation and nested callbacks')
