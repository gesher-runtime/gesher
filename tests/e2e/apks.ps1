# Cache only complete APKs matching the manifest's pin. A mirror changes the
# transport, never the bytes allowed into the test.
function Get-PinnedApk([string] $Path, [string] $FileName, [string] $Sha256, [string[]] $Repositories) {
  if (-not $Sha256) { throw "No SHA-256 pin for $FileName" }
  if (Test-Path -LiteralPath $Path) {
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash -eq $Sha256) { return }
    Write-Host "INFO  e2e: discarded cached $FileName with a mismatched SHA-256"
    Remove-Item -LiteralPath $Path -Force
  }
  $temporary = "$Path.partial"
  $problems = @()
  foreach ($repository in $Repositories) {
    $url = "$($repository.TrimEnd('/'))/$FileName"
    try {
      Invoke-WebRequest -Uri $url -OutFile $temporary -UseBasicParsing -TimeoutSec 60 -MaximumRetryCount 1 -RetryIntervalSec 5
      $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $temporary).Hash
      if ($hash -ne $Sha256) { throw "SHA-256 $hash, not the pinned $Sha256" }
      Move-Item -LiteralPath $temporary -Destination $Path
      return
    } catch {
      $problems += "${url}: $_"
      Write-Host "INFO  e2e: APK fetch failed: $($problems[-1])"
    } finally {
      if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
    }
  }
  throw ($problems -join '; ')
}
