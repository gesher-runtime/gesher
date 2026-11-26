param([Parameter(Mandatory=$true)][string]$BuildDirectory)
$ErrorActionPreference = 'Stop'
$build = (Resolve-Path -LiteralPath $BuildDirectory).Path
$key = 'gesher-rights-' + [Guid]::NewGuid().ToString('N')
$previousKey = $env:GESHER_BINDER
$env:GESHER_BINDER = $key
$result = 0
$broker = $null
try {
    $broker = Start-Process -FilePath (Join-Path $build 'binderd.exe') -ArgumentList $key -PassThru
    $null = $broker.Handle
    Start-Sleep -Milliseconds 500
    if ($broker.HasExited) { throw 'The diagnostic broker exited before the probes started.' }
    foreach ($name in @('last_close', 'transferred_close')) {
        $probe = Start-Process -FilePath (Join-Path $build "$name.exe") -NoNewWindow -PassThru
        $null = $probe.Handle
        try {
            if (!$probe.WaitForExit(20000)) {
                $probe.Kill()
                throw "$name exceeded its 20-second timeout."
            }
            if ($probe.ExitCode -ne 0) { $result = 1 }
        } finally {
            $probe.Dispose()
        }
    }
} finally {
    if ($null -ne $broker) {
        if (!$broker.HasExited) {
            $broker.Kill()
            $null = $broker.WaitForExit(5000)
        }
        $broker.Dispose()
    }
    $env:GESHER_BINDER = $previousKey
}
exit $result
