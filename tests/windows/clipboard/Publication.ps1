$ErrorActionPreference = 'Stop'
$source = (git rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or ($env:GITHUB_SHA -and $source -cne $env:GITHUB_SHA)) {
    throw 'Clipboard publication checkout does not match the executing workflow source'
}
Write-Host "CLIPBOARD_SOURCE source=$source run=$env:GITHUB_RUN_ID attempt=$env:GITHUB_RUN_ATTEMPT"
$inputs = @(
    'compat/windows/clipboard_publication.cc', 'compat/windows/clipboard_publication.h',
    'compat/windows/clipboard_dib_layout.h', 'compat/windows/clipboard_bridge.cc',
    'compat/windows/clipboard_bridge.h', 'tests/clipboard/publication_windows_test.cc',
    'tests/clipboard/wic_decode.h', 'tests/windows/clipboard/Publication.ps1'
)
foreach ($inputFile in $inputs) {
    $digest = (Get-FileHash -Algorithm SHA256 -LiteralPath $inputFile).Hash.ToLowerInvariant()
    Write-Host "CLIPBOARD_INPUT sha256=$digest path=$inputFile"
}
$executable = Join-Path (Get-Location) 'clipboard-publication.exe'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw 'Fresh native clipboard fixture was not built' }
$binaryDigest = (Get-FileHash -Algorithm SHA256 -LiteralPath $executable).Hash.ToLowerInvariant()
Write-Host "CLIPBOARD_BINARY sha256=$binaryDigest"
$stdout = Join-Path (Get-Location) 'clipboard-publication.stdout.log'
$stderr = Join-Path (Get-Location) 'clipboard-publication.stderr.log'
$child = Start-Process -FilePath $executable -PassThru -NoNewWindow -RedirectStandardOutput $stdout -RedirectStandardError $stderr
try {
    if (-not $child.WaitForExit(180000)) { throw 'Actual native clipboard fixture exceeded its 180-second process bound' }
    $child.WaitForExit()
    if ($child.ExitCode -ne 0) { throw "Native clipboard publication contract failed: $($child.ExitCode)" }
    $output = Get-Content -LiteralPath $stdout -Raw
    if ($output -notmatch 'Clipboard publication Windows completed: cases=[1-9][0-9]* checks=[1-9][0-9]* failures=0') {
        throw 'Native clipboard fixture did not report completion of its real contract checks'
    }
} finally {
    if (-not $child.HasExited) { $child.Kill(); $child.WaitForExit() }
    if (Test-Path -LiteralPath $stdout) { Get-Content -LiteralPath $stdout }
    if (Test-Path -LiteralPath $stderr) { Get-Content -LiteralPath $stderr }
    $child.Dispose()
}
