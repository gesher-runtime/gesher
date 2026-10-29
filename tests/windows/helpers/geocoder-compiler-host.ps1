# Build-only private tool; never added to guest/installer artifacts or PATH.
function New-GeocoderCompilerHost {
  if ($env:GITHUB_ACTIONS -cne 'true') { throw 'Compiler acquisition is CI only' }
  $directory = Join-Path $env:RUNNER_TEMP ('geocoder-tools-' + [Guid]::NewGuid().ToString('N'))
  $null = [IO.Directory]::CreateDirectory($directory)
  $script:geocoderToolsDirectory = [IO.Path]::GetFullPath($directory)
  $zipPath = Join-Path $directory 'compiler.zip'
  $compilerHome = Join-Path $directory 'pwsh'
  $url = 'https://github.com/PowerShell/PowerShell/releases/download/v7.6.9/PowerShell-7.6.9-win-x64.zip'
  $sha = 'ce7484804c0f0ad4dda10c958e71acdaf7eebfb22505c1b480f89e0e9f1da94d'
  # Official release 423544544, asset 675639558; also independently published in
  # hashes.sha256 (asset 675636979, SHA256 638e954664d07a29190570faf862b8a50b35f7e1c10dbd0f0e8a7e0f70138b45).
  # Stream the network response with both an overall deadline and a byte bound.
  $client = [Net.Http.HttpClient]::new()
  $cancel = [Threading.CancellationTokenSource]::new(120000)
  $response = $null; $network = $null; $file = $null
  try {
    $response = $client.GetAsync($url, [Net.Http.HttpCompletionOption]::ResponseHeadersRead, $cancel.Token).GetAwaiter().GetResult()
    $null = $response.EnsureSuccessStatusCode()
    if ($response.Content.Headers.ContentLength -ne 109620855) { throw 'Compiler package length mismatch' }
    $network = $response.Content.ReadAsStreamAsync($cancel.Token).GetAwaiter().GetResult()
    $file = [IO.FileStream]::new($zipPath, [IO.FileMode]::CreateNew)
    $buffer = [byte[]]::new(65536); $total = 0L
    while (($read = $network.ReadAsync($buffer, 0, $buffer.Length, $cancel.Token).GetAwaiter().GetResult()) -gt 0) {
      $total += $read
      if ($total -gt 109620855) { throw 'Compiler package exceeds bound' }
      $file.Write($buffer, 0, $read)
    }
    if ($total -ne 109620855) { throw 'Compiler package truncated' }
  } finally {
    if ($file) { $file.Dispose() }; if ($network) { $network.Dispose() }
    if ($response) { $response.Dispose() }; $cancel.Dispose(); $client.Dispose()
  }
  if ((Get-GeocoderSourceSha256 $zipPath) -cne $sha) { throw 'Compiler package SHA256 mismatch' }
  $null = [IO.Directory]::CreateDirectory($compilerHome)
  $prefix = [IO.Path]::GetFullPath($compilerHome).TrimEnd('\') + '\'
  $archive = [IO.Compression.ZipFile]::OpenRead($zipPath)
  try {
    if ($archive.Entries.Count -gt 5000) { throw 'Compiler archive entry bound' }
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $expanded = 0L
    foreach ($entry in $archive.Entries) {
      $name = $entry.FullName.Replace('/', '\')
      if ([IO.Path]::IsPathRooted($name) -or $name.Contains(':') -or @($name.Split('\') | Where-Object { $_ -in @('.', '..') -or $_ -match '[. ]$' }).Count -gt 0) { throw 'Unsafe compiler archive path' }
      $target = [IO.Path]::GetFullPath((Join-Path $compilerHome $name))
      if (-not $target.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -or -not $seen.Add($target)) { throw 'Compiler archive path escaped or duplicated' }
      if (($entry.ExternalAttributes -band 0x400) -ne 0 -or (($entry.ExternalAttributes -shr 16) -band 0xF000) -eq 0xA000) { throw 'Compiler archive link rejected' }
      $expanded += $entry.Length
      if ($entry.Length -gt 150000000 -or $expanded -gt 600000000) { throw 'Compiler archive expansion bound' }
      if ($name.EndsWith('\')) { $null = [IO.Directory]::CreateDirectory($target); continue }
      $null = [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
      $inputStream = $entry.Open(); $outputStream = $null
      try {
        $outputStream = [IO.FileStream]::new($target, [IO.FileMode]::CreateNew)
        $inputStream.CopyTo($outputStream)
        if ($outputStream.Length -ne $entry.Length) { throw 'Compiler archive entry length mismatch' }
      } finally { if ($outputStream) { $outputStream.Dispose() }; $inputStream.Dispose() }
    }
  } finally { $archive.Dispose() }
  $package = @{ Version = '7.6.9'; Home = $compilerHome; Url = $url; Sha256 = $sha; Bytes = $total; ReleaseId = '423544544'; AssetId = '675639558'; ExpandedBytes = $expanded; Entries = $seen.Count }
  Write-Host ('WINRT_PRIVATE_COMPILER_PACKAGE ' + ($package | ConvertTo-Json -Compress))
  return $package
}

function Remove-GeocoderCompilerHost {
  if ($script:geocoderToolsDirectory) {
    $directory = [IO.Path]::GetFullPath($script:geocoderToolsDirectory)
    $prefix = [IO.Path]::GetFullPath($env:RUNNER_TEMP).TrimEnd('\') + '\'
    if (-not $directory.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($directory) -notmatch '^geocoder-tools-[0-9a-f]{32}$') { throw 'Invalid compiler tool cleanup path' }
    # Only this unique owned directory receives hash-verified archive entries;
    # extraction rejects links and traversal before creating any entry.
    Remove-Item -LiteralPath $directory -Recurse -Force -ErrorAction Stop
    if ([IO.Directory]::Exists($directory)) { throw 'Private compiler tool cleanup failed' }
    $script:geocoderToolsDirectory = $null
    Write-Host 'WINRT_PRIVATE_COMPILER_CLEANUP directory_removed=True'
  }
}
