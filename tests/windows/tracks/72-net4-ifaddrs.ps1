# JNI/APK paths are required by 44-native2's new ifaddrs part in both ABIs
# and both app modes. This standalone ELF uses an independent Windows snapshot.
. (Join-Path $PSScriptRoot '../net/IfaddrsOracle.ps1')
[string[]]$oracleRows = @(Get-Net4IfaddrsOracleRows)
$oracle = Join-Path $env:RUNNER_TEMP "net4-ifaddrs-$([guid]::NewGuid()).txt"
[IO.File]::WriteAllLines($oracle, $oracleRows, [Text.UTF8Encoding]::new($false))
try {
  foreach ($abi in 'x86_64', 'arm64-v8a') {
    $libraryPath = NoDrive "$n2/$abi"
    $out = N2Run "$n2Dir/gesher-exec.exe" @('--elf', "$n2/$abi/net4_ifaddrs_test", 'net4_ifaddrs_test', (NoDrive $oracle)) `
      (Get-Location).Path @{ LD_LIBRARY_PATH = $libraryPath } 90 | Out-String
    $code = $LASTEXITCODE
    Write-Host $out
    if ($code -eq 0 -and $out -notmatch '(?m)^FAIL' -and
        $out -match 'net4 ifaddrs: all passed; .*host oracle ([0-9]+)' -and [int]$Matches[1] -ge 3) {
      Write-Host "PASS  net4 ifaddrs ($abi): standalone ELF matches actual Windows addresses, prefixes, scopes, hardware/index and independent allocation lifetime"
    } else {
      Write-Host "FAIL  net4 ifaddrs ($abi): standalone ELF/oracle (exit $code)"
      $script:failed++
    }
  }
} finally {
  Remove-Item -LiteralPath $oracle -ErrorAction SilentlyContinue
}
