# Test-only host client for the inherited anonymous pipe to trusted_sg_owner.py.
# Guest processes receive neither this pipe nor the broker's private endpoint.
if (-not ('TrustedSgJob' -as [type])) { Add-Type -Path (Join-Path $PSScriptRoot 'TrustedSgJob.cs') }
function Start-TrustedSgOwner {
    $python = (Get-Command python -CommandType Application).Source
    $directory = Join-Path $env:RUNNER_TEMP ('sg-owner-' + [guid]::NewGuid().ToString('N'))
    $security = [Security.AccessControl.DirectorySecurity]::new()
    $security.SetAccessRuleProtection($true, $false)
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent().User
    $rule = [Security.AccessControl.FileSystemAccessRule]::new($identity, 'FullControl', 'ContainerInherit,ObjectInherit', 'None', 'Allow')
    $security.SetOwner($identity)
    $security.AddAccessRule($rule)
    [void][IO.FileSystemAclExtensions]::CreateDirectory([IO.DirectoryInfo]::new($directory), $security)
    if (([IO.File]::GetAttributes($directory) -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'SG owner directory is a reparse point' }
    $native = [TrustedSgJob]::Start($python, (Join-Path $PSScriptRoot 'trusted_sg_owner.py'), $directory)
    return [pscustomobject]@{ Native = $native; Directory = $directory; Error = $native.Error.ReadToEndAsync(); Closed = $false; Faulted = $false }

}

function Invoke-TrustedSgOwner($Owner, [hashtable]$Request) {
  try {
    $json = ConvertTo-Json -InputObject $Request -Depth 8 -Compress
    if ([Text.Encoding]::UTF8.GetByteCount($json) -gt 65535) { throw 'SG owner request exceeds bound' }
    $Owner.Native.Input.WriteLine($json)
    $Owner.Native.Input.Flush()
    $reply = $Owner.Native.Output.ReadLineAsync()
    if (-not $reply.Wait(15000)) { throw 'SG owner response deadline expired' }
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
        Write-Host $Owner.Error.Result
        [TrustedSgJob]::CloseProcess($Owner.Native)
        [TrustedSgJob]::CloseJob($Owner.Native)
        $Owner.Native.Input.Dispose()
        $Owner.Native.Output.Dispose()
        $Owner.Native.Error.Dispose()
        $items = @(Get-ChildItem -LiteralPath $Owner.Directory -Force)
        foreach ($item in $items) {
            if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
                $item.Name -notmatch '^[1-9][0-9]*\.log$') { throw 'unexpected SG owner log entry; preserved' }
            Remove-Item -LiteralPath $item.FullName -Force
        }
        Remove-Item -LiteralPath $Owner.Directory -Force
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
