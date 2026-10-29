param([switch] $OwnedConsole)
$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -cne 'true') { throw 'CI only' }
Add-Type -Path tools/windows4/TsfJobSupervisor.cs
Write-Host ('PWSH_CONSOLE_DETAILS ' + [TsfJobSupervisor]::TestConsoleDetails())
$root = Join-Path $env:RUNNER_TEMP ('pwsh-job-diag-' + [guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($root)
$script = Join-Path $root 'child.ps1'
@'
param([string] $Marker)
Add-Type -TypeDefinition 'using System; using System.Runtime.InteropServices; public static class ConsoleMembers { [DllImport("kernel32.dll", SetLastError=true)] public static extern uint GetConsoleProcessList([Out] uint[] ids, uint count); }'
$ids = [uint32[]]::new(256)
$count = [ConsoleMembers]::GetConsoleProcessList($ids, [uint32]$ids.Length)
$members = if ($count -gt 0 -and $count -le $ids.Length) { ($ids[0..($count-1)] -join ',') } else { '<unavailable>' }
[IO.File]::WriteAllText($Marker, ([Environment]::CommandLine + " pid=$PID count=$count members=$members"))
[Console]::Out.WriteLine('DIAG_STDOUT')
[Console]::Error.WriteLine('DIAG_STDERR')
'@ | Set-Content -LiteralPath $script -Encoding utf8
try {
  $lease = $null
  if ($OwnedConsole) {
    $lease = [TsfJobSupervisor]::TestCreateConsole()
    $signature = Get-AuthenticodeSignature -LiteralPath $lease.Image
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'CN=Microsoft Windows') { throw 'Console host publisher not verified' }
    Write-Host "PWSH_CONSOLE_LEASE pid=$($lease.Pid) birth=$($lease.Birth) image=$($lease.Image)"
  }
  $exe = [Environment]::ProcessPath
  Write-Host "PWSH_PARENT_CONSOLE actual=$([TsfJobSupervisor]::TestParentConsole())"
  foreach ($variant in @('production','diag-detached-unlimited','diag-console-limited','diag-console-unlimited','diag-inherit-limited','diag-inherit-unlimited')) {
    if ($lease -and -not [TsfJobSupervisor]::TestConsoleLease($lease)) { throw 'Console association changed' }
    $marker = Join-Path $root ($variant + '.marker')
    $arguments = '-NoProfile -ExecutionPolicy Bypass -File "' + $script + '" "' + $marker + '"'
    $fault = if ($variant -eq 'production') { '' } else { $variant }
    Write-Host "PWSH_DIAGNOSTIC variant=$variant exe=$exe args=$arguments script_sha256=$((Get-FileHash -LiteralPath $script -Algorithm SHA256).Hash) stdin=NUL stdout=owned-file stderr=owned-file"
    $r = [TsfJobSupervisor]::Run($exe, $arguments, 5000, $fault)
    $command = if (Test-Path -LiteralPath $marker) { Get-Content -LiteralPath $marker -Raw } else { '<missing>' }
    Write-Host "PWSH_DIAGNOSTIC_RESULT variant=$variant marker=$command"
    Write-Host ('PWSH_DIAGNOSTIC_JOB ' + ($r | ConvertTo-Json -Depth 6 -Compress))
    Write-Host ('PWSH_CONSOLE_AFTER variant=' + $variant + ' ' + [TsfJobSupervisor]::TestConsoleDetails())
  }
} finally {
  if ($lease) { [TsfJobSupervisor]::TestCloseConsole($lease); Write-Host 'PWSH_CONSOLE_LEASE retired=true' }
  Get-ChildItem -LiteralPath $root -File | Remove-Item
  Remove-Item -LiteralPath $root
}
