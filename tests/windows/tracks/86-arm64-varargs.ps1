# ADR0079: actual original logger, standalone C consumers and API3 rejection.
# Native2 additionally requires the same contract body in dalvikvm and both
# actual APK contexts on both ABIs; the original Arm64Jni V assertions retain
# their primitive/mixed values and add independent copied-list calls.
try {
  . (Join-Path $PSScriptRoot '../run-arm64-varargs.ps1') -Dir (Resolve-Path $Dir).Path
  Write-Host 'PASS  arm64 varargs: all original logger, standalone consumer and API rejection groups completed'
} catch {
  Write-Host "FAIL  arm64 varargs: $_"
  $failed++
}
