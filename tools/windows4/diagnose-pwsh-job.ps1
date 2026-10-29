$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -cne 'true') { throw 'CI only' }
Add-Type -Path tools/windows4/TsfJobSupervisor.cs
$root = Join-Path $env:RUNNER_TEMP ('pwsh-job-diag-' + [guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($root)
$script = Join-Path $root 'child.ps1'
@'
param([string] $Marker)
[IO.File]::WriteAllText($Marker, [Environment]::CommandLine)
[Console]::Out.WriteLine('DIAG_STDOUT')
[Console]::Error.WriteLine('DIAG_STDERR')
'@ | Set-Content -LiteralPath $script -Encoding utf8
try {
  $exe = [Environment]::ProcessPath
  Write-Host "PWSH_PARENT_CONSOLE actual=$([TsfJobSupervisor]::TestParentConsole())"
  foreach ($variant in @('production','diag-detached-unlimited','diag-console-limited','diag-console-unlimited','diag-inherit-limited','diag-inherit-unlimited')) {
    $marker = Join-Path $root ($variant + '.marker')
    $arguments = '-NoProfile -ExecutionPolicy Bypass -File "' + $script + '" "' + $marker + '"'
    $fault = if ($variant -eq 'production') { '' } else { $variant }
    Write-Host "PWSH_DIAGNOSTIC variant=$variant exe=$exe args=$arguments script_sha256=$((Get-FileHash -LiteralPath $script -Algorithm SHA256).Hash) stdin=NUL stdout=owned-file stderr=owned-file"
    $r = [TsfJobSupervisor]::Run($exe, $arguments, 5000, $fault)
    $command = if (Test-Path -LiteralPath $marker) { Get-Content -LiteralPath $marker -Raw } else { '<missing>' }
    Write-Host "PWSH_DIAGNOSTIC_RESULT variant=$variant marker=$command"
    Write-Host ('PWSH_DIAGNOSTIC_JOB ' + ($r | ConvertTo-Json -Depth 6 -Compress))
  }
} finally {
  Get-ChildItem -LiteralPath $root -File | Remove-Item
  Remove-Item -LiteralPath $root
}
