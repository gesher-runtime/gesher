param([Parameter(Mandatory)] [string] $Dir)
# Preserve every Native2/FPCR/FPSR/varargs/JNI assertion and append test85.
. "$PSScriptRoot/run-arm64-component.ps1" -Dir $Dir -ContinueAfterComponent
. "$PSScriptRoot/tracks/85-arm64-environment.ps1"
Write-Host "ACTUAL_ARM64_ENVIRONMENT_SUMMARY $failed test(s) failed; original coherent component plus complete environment/default object"
if ($failed) { exit 1 }
exit 0
