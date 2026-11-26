# Run the signed APK against a peer with its own Windows network compartment.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $RuntimeDir,
    [Parameter(Mandatory)][string] $AndroidSdk,
    [Parameter(Mandatory)][string] $OutputDir
)

$ErrorActionPreference = 'Stop'
$out = (New-Item -ItemType Directory -Path $OutputDir -Force).FullName
$peerName = 'gesher-lan-' + [guid]::NewGuid().ToString('N')
$port = 18765
$marker = 'Gesher-CI-Network-Probe'
$server = @'
$ErrorActionPreference = 'Stop'
$listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Any, 18765)
$listener.Start()
try {
    while ($true) {
        $client = $listener.AcceptTcpClient()
        try {
            Write-Output "Accepted $($client.Client.RemoteEndPoint)"
            $stream = $client.GetStream()
            $stream.ReadTimeout = 4000
            $stream.WriteTimeout = 4000
            $reader = [IO.StreamReader]::new($stream, [Text.Encoding]::ASCII)
            $request = $reader.ReadLine()
            for ($i = 0; $i -lt 64; $i++) {
                if ([string]::IsNullOrEmpty($reader.ReadLine())) { break }
            }
            if ($request -notmatch '^GET /probe\.txt HTTP/1\.[01]$') { continue }
            $body = 'Gesher-CI-Network-Probe'
            $response = @('HTTP/1.1 200 OK', 'Content-Type: text/plain', "Content-Length: $($body.Length)",
                          'Cache-Control: no-store', 'Connection: close', '', $body) -join [Environment]::NewLine
            $bytes = [Text.Encoding]::ASCII.GetBytes($response)
            $stream.Write($bytes, 0, $bytes.Length)
            Write-Output "$([DateTime]::UtcNow.ToString('o')) $($client.Client.RemoteEndPoint) $request"
        } catch [IO.IOException] {
            Write-Output "Incomplete request: $($_.Exception.Message)"
        } finally {
            $client.Dispose()
        }
    }
} finally {
    $listener.Stop()
}
'@
$encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($server))

function Read-PeerEndpoint {
    $raw = & docker inspect --format '{{json .NetworkSettings.Networks.nat}}' $peerName
    if ($LASTEXITCODE -ne 0) { throw 'Could not inspect the Windows NAT peer' }
    $endpoint = $raw | ConvertFrom-Json
    [Net.IPAddress] $peerIp = $null
    [Net.IPAddress] $hostIp = $null
    if (![Net.IPAddress]::TryParse($endpoint.IPAddress, [ref] $peerIp) -or
        ![Net.IPAddress]::TryParse($endpoint.Gateway, [ref] $hostIp) -or
        $peerIp.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork -or
        $hostIp.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork) {
        throw 'The Windows NAT network did not assign IPv4 addresses to the peer and its gateway'
    }
    $interfaces = @(Get-NetIPAddress -AddressFamily IPv4)
    if ($peerIp.IPAddressToString -in $interfaces.IPAddress -or
        $hostIp.IPAddressToString -notin $interfaces.IPAddress) {
        throw 'Expected a distinct peer and a gateway address owned by the Windows runner'
    }
    $octets = $peerIp.GetAddressBytes()
    if (!($octets[0] -eq 10 -or ($octets[0] -eq 172 -and $octets[1] -in (16..31)) -or
          ($octets[0] -eq 192 -and $octets[1] -eq 168))) {
        throw "Windows NAT peer did not receive a private IPv4 address: $peerIp"
    }
    return $peerIp.IPAddressToString, $hostIp.IPAddressToString
}

function Wait-HttpFixture([uri] $Url) {
    $deadline = [DateTime]::UtcNow.AddSeconds(40)
    do {
        $body = & curl.exe --noproxy '*' --silent --fail --connect-timeout 2 --max-time 3 $Url.AbsoluteUri
        if ($LASTEXITCODE -eq 0 -and $body -ceq $marker) { return }
        Start-Sleep -Milliseconds 500
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Ordinary Windows HTTP could not read the expected fixture from $Url"
}

$hostServer = $null
$peerStarted = $false
try {
    $engine = & docker info --format '{{.OSType}}'
    if ($LASTEXITCODE -ne 0 -or $engine -ne 'windows') { throw 'A Windows Docker engine is required for the LAN peer' }
    & docker run --detach --name $peerName --network nat --isolation process mcr.microsoft.com/windows/servercore:ltsc2025 powershell.exe -NoProfile -EncodedCommand $encoded
    if ($LASTEXITCODE -ne 0) { throw 'Could not start the Windows LAN peer' }
    $peerStarted = $true
    $peerIp, $hostIp = Read-PeerEndpoint
    $peerUrl = "http://${peerIp}:${port}/probe.txt"
    $hostUrl = "http://${hostIp}:${port}/probe.txt"
    $loopbackUrl = "http://127.0.0.1:${port}/probe.txt"
    $hostServer = Start-Process powershell.exe -WindowStyle Hidden -PassThru `
        -ArgumentList '-NoProfile', '-EncodedCommand', $encoded `
        -RedirectStandardOutput (Join-Path $out 'host-http.log') `
        -RedirectStandardError (Join-Path $out 'host-http-error.log')
    Wait-HttpFixture $peerUrl
    Wait-HttpFixture $hostUrl
    Wait-HttpFixture $loopbackUrl
    if ($hostServer.HasExited) { throw 'The host HTTP fixture process exited during setup' }

    $apkDir = Join-Path $out 'apk'
    $fixture = @{
        AndroidSdk = $AndroidSdk
        OutputDir = $apkDir
        PublicUrl = 'https://www.iana.org/domains/reserved'
        PublicMarker = 'IANA-managed Reserved Domains'
        HostUrl = $hostUrl
        HostMarker = $marker
        LoopbackUrl = $loopbackUrl
        LanUrl = $peerUrl
        LanMarker = $marker
    }
    & "$PSScriptRoot/Build-SandboxNetworkApk.ps1" @fixture
    $connections = @{}
    $observe = {
        param([string] $Phase)
        $peerLog = @(& docker logs $peerName)
        if ($LASTEXITCODE -ne 0) { throw 'Could not read the LAN peer connection log' }
        $hostLog = @(Get-Content -LiteralPath (Join-Path $out 'host-http.log'))
        $connections[$Phase] = @(
            @($peerLog -match '^Accepted ').Count
            @($hostLog -match '^Accepted ').Count
        )
        if ($Phase -eq 'after' -and ($connections.before -join ',') -ne ($connections.after -join ',')) {
            throw "A restricted listener accepted a connection during the contained run. Before: $($connections.before). After: $($connections.after)."
        }
    }
    & "$PSScriptRoot/Test-SandboxNetworkApk.ps1" -RuntimeDir $RuntimeDir `
        -Apk (Join-Path $apkDir 'sandbox-network.apk') -ScratchDir $out -WithLoopback -WithLanPeer -ObserveContained $observe
    Wait-HttpFixture $peerUrl
} finally {
    if ($null -ne $hostServer -and !$hostServer.HasExited) {
        Stop-Process -InputObject $hostServer -ErrorAction SilentlyContinue
    }
    if ($peerStarted) {
        & docker logs $peerName *> (Join-Path $out 'lan-peer-http.log')
        & docker rm --force $peerName | Out-Null
    }
}
