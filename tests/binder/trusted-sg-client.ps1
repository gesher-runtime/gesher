# Test-only host client for the inherited anonymous pipe to trusted_sg_owner.py.
# Guest processes receive neither this pipe nor the broker's private endpoint.
if (-not ('TrustedSgJob' -as [type])) { Add-Type -Path (Join-Path $PSScriptRoot 'TrustedSgJob.cs') }
function Start-TrustedSgOwner {
    $command = Get-Command python -CommandType Application | Select-Object -First 1
    if ($null -eq $command -or -not [IO.Path]::IsPathFullyQualified($command.Source) -or
        -not [IO.File]::Exists($command.Source)) { throw 'Python executable path missing or ambiguous' }
    $python = $command.Source
    $script = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot 'trusted_sg_owner.py')).Path
    Write-Host "SG_OWNER_EXECUTABLE path=$python sha256=$((Get-FileHash -LiteralPath $python -Algorithm SHA256).Hash.ToLowerInvariant()) script=$script"
    $native = [TrustedSgJob]::Start($python, $script, 0)
    return [pscustomobject]@{ Native = $native; Error = $native.Error.ReadToEndAsync(); Pending = $null; Closed = $false; Faulted = $false }

}

function Invoke-TrustedSgOwner($Owner, [hashtable]$Request) {
  try {
    $json = ConvertTo-Json -InputObject $Request -Depth 8 -Compress
    if ([Text.Encoding]::UTF8.GetByteCount($json) -gt 65535) { throw 'SG owner request exceeds bound' }
    $Owner.Native.Input.WriteLine($json)
    $Owner.Native.Input.Flush()
    $reply = $Owner.Native.Output.ReadLineAsync()
    $Owner.Pending = $reply
    if (-not $reply.Wait(15000)) { throw 'SG owner response deadline expired' }
    $Owner.Pending = $null
    if ($null -eq $reply.Result -or $reply.Result.Length -gt 50000) { throw 'invalid SG owner response' }
    $value = $reply.Result | ConvertFrom-Json
    if (-not $value.ok) { throw 'SG owner refused request' }
    return $value
  } catch { $Owner.Faulted = $true; throw }
}

function Close-TrustedSgOwner($Owner) {
    if ($Owner.Closed) { return }
    $problem = $null
    try {
      try {
        if (-not $Owner.Faulted) {
            try {
                [void](Invoke-TrustedSgOwner $Owner @{ op = 'close' })
                $Owner.Native.Input.Close()
                if (-not [TrustedSgJob]::Wait($Owner.Native, 15000)) { throw 'SG owner cleanup deadline expired' }
                if (-not $Owner.Error.Wait(5000)) { throw 'SG owner stderr drain deadline expired' }
                Write-Host $Owner.Error.Result
                if ([TrustedSgJob]::ExitCode($Owner.Native) -ne 0) { throw "SG owner exited $([TrustedSgJob]::ExitCode($Owner.Native))" }
            } catch { $problem = $_ }
        } else { $problem = 'SG owner protocol failed' }
    } finally {
        if ([TrustedSgJob]::Active($Owner.Native.Job) -ne 0) {
            [TrustedSgJob]::Terminate($Owner.Native.Job)
            $deadline = [Diagnostics.Stopwatch]::StartNew()
            while ([TrustedSgJob]::Active($Owner.Native.Job) -ne 0 -and $deadline.ElapsedMilliseconds -lt 10000) {
                Start-Sleep -Milliseconds 20
            }
        }
        if ([TrustedSgJob]::Active($Owner.Native.Job) -ne 0) { throw 'SG job cleanup unconfirmed; job handle retained' }
        if (-not [TrustedSgJob]::Wait($Owner.Native, 5000)) { throw 'SG owner exact process not signaled; handles retained' }
        if (-not $Owner.Error.Wait(5000)) { throw 'SG owner stderr drain unconfirmed; handles retained' }
        if ($null -ne $Owner.Pending -and -not $Owner.Pending.Wait(5000)) { throw 'SG owner response drain unconfirmed; handles retained' }
        $Owner.Pending = $null
        Write-Host $Owner.Error.Result
        [TrustedSgJob]::CloseProcess($Owner.Native)
        [TrustedSgJob]::CloseJob($Owner.Native)
        [TrustedSgJob]::CloseStreams($Owner.Native)
        $Owner.Closed = $true
      }
      } catch {
        [TrustedSgJob]::QuarantineOwner($Owner.Native)
        throw
      }
    if ($problem) { throw $problem }
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
