# The real launcher's shell, not a shell mock. Each case has an exact output
# and status assertion; pipelines exceed a pipe buffer, subshells preserve
# state without inheriting mutations, and serialized state exceeds 32 KiB.
$exec3ShellLog = & python "$PSScriptRoot/../../exec/shell-smoke.py" "$Dir/gesher-exec.exe" 2>&1
$exec3ShellCode = $LASTEXITCODE
$exec3ShellLog | ForEach-Object { Write-Host $_ }
if ($exec3ShellCode -ne 0) {
  Write-Host "FAIL  exec3: shell regression suite (exit $exec3ShellCode)"
  $failed++
} else {
  Write-Host 'PASS  exec3: shell regression suite through the real launcher'
}

Check 'exec3: ProcessBuilder shell with an open stdin (incremental output, read and multiline commands)' {
  & "$Dir/dalvikvm.exe" "-Xbootclasspath:$bcp" "-Ximage:$(NoDrive "$root/system")/framework/boot.art" `
    -Xint '-Xusejit:false' -cp (NoDrive "$Dir/exec-test.dex") gesher.Exec3Test
} @('Exec3Test PASS incremental stdin, read, multiline loop, pipeline, substitution, EXIT trap and status')
