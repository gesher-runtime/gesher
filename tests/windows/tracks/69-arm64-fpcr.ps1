# ADR 0067's first slice: actual translated FPCR and native host boundaries.
Check 'arm64-2: real FPCR rounding, nested callbacks, thread isolation and restored native host fenv' {
  A64Run "$a64/arm64_fpcr_test.exe" @("$a64/arm64-fpcr-code.bin")
} @('PASS  arm64 FPCR: four modes, real positive/negative arithmetic and preserved FPCR bits',
  'PASS  arm64 FPCR: invalid mode rejection preserves actual state',
  'PASS  arm64 FPCR: direct MSR and nested guest-host callbacks inherit/persist state and restore host fenv',
  'PASS  arm64 FPCR: concurrent guest thread isolation and native host fenv restoration',
  '0 test(s) failed')
