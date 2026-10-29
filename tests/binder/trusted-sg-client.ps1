# Test-only host client for the inherited anonymous pipe to trusted_sg_owner.py.
# Guest processes receive neither this pipe nor the broker's private endpoint.
function Start-TrustedSgOwner {
    $info = [Diagnostics.ProcessStartInfo]::new('python')
    $info.ArgumentList.Add((Join-Path $PSScriptRoot 'trusted_sg_owner.py'))
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $process = [Diagnostics.Process]::Start($info)
    if ($null -eq $process) { throw 'trusted SG owner did not start' }
    return [pscustomobject]@{ Process = $process; Error = $process.StandardError.ReadToEndAsync(); Closed = $false }
}

function Invoke-TrustedSgOwner($Owner, [hashtable]$Request) {
    $json = ConvertTo-Json -InputObject $Request -Depth 8 -Compress
    if ([Text.Encoding]::UTF8.GetByteCount($json) -gt 65535) { throw 'SG owner request exceeds bound' }
    $Owner.Process.StandardInput.WriteLine($json)
    $Owner.Process.StandardInput.Flush()
    $reply = $Owner.Process.StandardOutput.ReadLineAsync()
    if (-not $reply.Wait(15000)) { throw 'SG owner response deadline expired' }
    if ($null -eq $reply.Result -or $reply.Result.Length -gt 50000) { throw 'invalid SG owner response' }
    $value = $reply.Result | ConvertFrom-Json
    if (-not $value.ok) { throw 'SG owner refused request' }
    return $value
}

function Close-TrustedSgOwner($Owner) {
    if ($Owner.Closed) { return }
    try {
        [void](Invoke-TrustedSgOwner $Owner @{ op = 'close' })
        $Owner.Process.StandardInput.Close()
        if (-not $Owner.Process.WaitForExit(15000)) { throw 'SG owner cleanup deadline expired' }
        if (-not $Owner.Error.Wait(5000)) { throw 'SG owner stderr drain deadline expired' }
        Write-Host $Owner.Error.Result
        if ($Owner.Process.ExitCode -ne 0) { throw "SG owner exited $($Owner.Process.ExitCode)" }
    } finally {
        if (-not $Owner.Process.HasExited) {
            $Owner.Process.Kill($true)
            if (-not $Owner.Process.WaitForExit(5000)) { throw 'SG owner forced cleanup failed' }
        }
        $Owner.Process.Dispose()
        $Owner.Closed = $true
    }
}

function Read-TrustedSgOutput($Owner, [int]$Id) {
    $offset = 0
    $bytes = [IO.MemoryStream]::new()
    try {
        do {
            $response = Invoke-TrustedSgOwner $Owner @{ op = 'read'; id = $Id; offset = $offset }
            $chunk = [Convert]::FromBase64String($response.data)
            if ($response.next -ne ($offset + $chunk.Length)) { throw 'invalid SG output offset' }
            $bytes.Write($chunk, 0, $chunk.Length)
            $offset = [int]$response.next
            if ($offset -gt 16MB) { throw 'SG output exceeds bound' }
        } while ($chunk.Length -eq 32768)
        return [Text.Encoding]::UTF8.GetString($bytes.ToArray())
    } finally { $bytes.Dispose() }
}
