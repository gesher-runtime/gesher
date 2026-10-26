# Networking (docs/network.md, ADR 0037): Java's sockets, DNS and NIO over
# gesher-base, BoringSSL (NativeBN, Conscrypt), OkHttp behind HttpURLConnection,
# the system CA certificates from Windows' store, and HTTPS on the internet.
# Dot-sourced by run-tests.ps1: Check, RunDex, NoDrive, $Dir, $root, $bcp and
# $failed are its.

# Runs gesher.NetTest (net-tests.dex, tests/dex-net) on the core boot class path,
# with a time limit; each of its "OK"/"FAIL" lines becomes a PASS/FAIL line here.
function NetDex([string] $Name, [string[]] $Arguments, [int] $Seconds = 300, [hashtable] $Environment = @{}) {
  if (-not (Test-Path "$Dir/net-tests.dex")) {
    Write-Host "FAIL  net: $Name (net-tests.dex is missing)"
    $script:failed++
    return ''
  }
  $psi = [Diagnostics.ProcessStartInfo]::new("$Dir/dalvikvm.exe")
  foreach ($a in @("-Xbootclasspath:$bcp", "-Ximage:$(NoDrive "$root/system")/framework/boot.art",
                   '-Xint', '-Xusejit:false', '-cp', (NoDrive "$Dir/net-tests.dex"), 'gesher.NetTest') + $Arguments) {
    $psi.ArgumentList.Add($a)
  }
  $psi.Environment['GESHER_TRACE_FAULTS'] = '1'
  $psi.Environment['GESHER_TRACE_CACERTS'] = '1'
  foreach ($k in $Environment.Keys) { $psi.Environment[$k] = $Environment[$k] }
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $watch = [Diagnostics.Stopwatch]::StartNew()
  $p = [Diagnostics.Process]::Start($psi)
  $stdout = $p.StandardOutput.ReadToEndAsync()
  $stderr = $p.StandardError.ReadToEndAsync()
  $timedOut = -not $p.WaitForExit($Seconds * 1000)
  if ($timedOut) { $p.Kill($true); $p.WaitForExit() }
  $null = $stdout.Wait(10000), $stderr.Wait(10000)
  $code = if ($timedOut) { -1 } else { $p.ExitCode }
  $out = $stdout.Result + "`n" + $stderr.Result
  if ($timedOut) { $out += "`n(timed out after $Seconds s)" }
  foreach ($line in $out -split "`r?`n") {
    if ($line -match '^OK    (.*)$') { Write-Host "PASS  net: $($Matches[1])" }
    elseif ($line -match '^FAIL  (.*)$') { Write-Host "FAIL  net: $($Matches[1])"; $script:failed++ }
    elseif ($line -match '^gesher: system CA certificates') { Write-Host "INFO  $line" }
  }
  if ($code -eq 0 -and $out.Contains('NetTest all passed')) {
    Write-Host ("PASS  net: {0} ({1:N1} s)" -f $Name, $watch.Elapsed.TotalSeconds)
  } else {
    Write-Host ("FAIL  net: {0} (exit 0x{1:X8})" -f $Name, $code)
    if (Test-Path "$Dir/dalvikvm.map") {
      $out | python "$PSScriptRoot/../../../tools/symbolize.py" "$Dir/dalvikvm.map" | Out-Host
    } else {
      Write-Host $out
    }
    $script:failed++
  }
  return $out
}

$cacerts = Join-Path $root 'system/etc/security/cacerts'

# On this machine: DNS, TCP and UDP on loopback, NIO, HTTP through OkHttp, BigInteger
# (NativeBN), the JCA providers (Conscrypt, BouncyCastle) and the default SSLContext.
$null = NetDex 'sockets, DNS, NIO, HTTP and crypto on this machine' @('local')

# The system CA certificates: Android's own and Windows' roots, in the directory
# Conscrypt and the network security config read.
$files = @(Get-ChildItem $cacerts -ErrorAction SilentlyContinue | Where-Object Name -match '^[0-9a-f]{8}\.\d+$')
$bundled = @(Get-ChildItem "$Dir/cacerts" -ErrorAction SilentlyContinue).Count
if ($bundled -gt 100 -and $files.Count -ge $bundled -and (Test-Path (Join-Path $cacerts '.gesher-cacerts'))) {
  Write-Host "PASS  net: the system CA certificates in `$ANDROID_ROOT/etc/security/cacerts: $($files.Count) ($bundled Android's, the rest from Windows' stores)"
} else {
  Write-Host "FAIL  net: the system CA certificates in `$ANDROID_ROOT/etc/security/cacerts: $($files.Count) files, $bundled bundled"
  $failed++
}

# TCP and UDP echo servers that are not Java: .NET's, on another thread of this script.
$tcp = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
$tcp.Start()
$echoPort = $tcp.LocalEndpoint.Port
$udp = [Net.Sockets.UdpClient]::new([Net.IPEndPoint]::new([Net.IPAddress]::Loopback, $echoPort))
$echoJobs = @(
  Start-ThreadJob -ArgumentList $tcp -ScriptBlock {
    param($listener)
    while ($true) {
      $c = $listener.AcceptTcpClient()
      try {
        $s = $c.GetStream()
        $buffer = [byte[]]::new(65536)
        while (($n = $s.Read($buffer, 0, $buffer.Length)) -gt 0) { $s.Write($buffer, 0, $n) }
      } catch { } finally { $c.Close() }
    }
  }
  Start-ThreadJob -ArgumentList $udp -ScriptBlock {
    param($client)
    while ($true) {
      $from = [Net.IPEndPoint]::new([Net.IPAddress]::Any, 0)
      $data = $client.Receive([ref]$from)
      $null = $client.Send($data, $data.Length, $from)
    }
  })
try {
  $null = NetDex "TCP and UDP echo by .NET servers on 127.0.0.1:$echoPort" @('echo', "$echoPort") 120
} finally {
  $tcp.Stop(); $udp.Close()
  $echoJobs | Stop-Job -PassThru | Remove-Job -Force
}

# A root installed in Windows (as a company or a content filter installs theirs) is
# trusted by Android apps: an HTTPS server on 127.0.0.1 with a certificate for
# localhost from a test root is rejected until the root is in the machine's ROOT
# store, trusted once it is, and rejected again once it is removed.
Add-Type -AssemblyName System.Security
$now = [DateTimeOffset]::UtcNow
$rootKey = [Security.Cryptography.RSA]::Create(2048)
$rootReq = [Security.Cryptography.X509Certificates.CertificateRequest]::new(
  'CN=Gesher Net Test Root, O=Gesher CI', $rootKey, [Security.Cryptography.HashAlgorithmName]::SHA256,
  [Security.Cryptography.RSASignaturePadding]::Pkcs1)
$rootReq.CertificateExtensions.Add([Security.Cryptography.X509Certificates.X509BasicConstraintsExtension]::new($true, $false, 0, $true))
$rootReq.CertificateExtensions.Add([Security.Cryptography.X509Certificates.X509KeyUsageExtension]::new(
  [Security.Cryptography.X509Certificates.X509KeyUsageFlags]'KeyCertSign, CrlSign, DigitalSignature', $true))
$rootReq.CertificateExtensions.Add([Security.Cryptography.X509Certificates.X509SubjectKeyIdentifierExtension]::new($rootReq.PublicKey, $false))
$rootCert = $rootReq.CreateSelfSigned($now.AddDays(-1), $now.AddDays(30))
$leafKey = [Security.Cryptography.RSA]::Create(2048)
$leafReq = [Security.Cryptography.X509Certificates.CertificateRequest]::new(
  'CN=localhost', $leafKey, [Security.Cryptography.HashAlgorithmName]::SHA256,
  [Security.Cryptography.RSASignaturePadding]::Pkcs1)
$san = [Security.Cryptography.X509Certificates.SubjectAlternativeNameBuilder]::new()
$san.AddDnsName('localhost')
$san.AddIpAddress([Net.IPAddress]::Loopback)
$leafReq.CertificateExtensions.Add($san.Build())
$eku = [Security.Cryptography.OidCollection]::new()
$null = $eku.Add([Security.Cryptography.Oid]::new('1.3.6.1.5.5.7.3.1'))
$leafReq.CertificateExtensions.Add([Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]::new($eku, $false))
$leafReq.CertificateExtensions.Add([Security.Cryptography.X509Certificates.X509BasicConstraintsExtension]::new($false, $false, 0, $true))
$serial = [byte[]]::new(16); [Security.Cryptography.RandomNumberGenerator]::Fill($serial); $serial[0] = 0x42
$leaf = $leafReq.Create($rootCert, $now.AddDays(-1), $now.AddDays(29), $serial)
# SChannel wants the key in a key container: through a PFX.
$leafPfx = [Security.Cryptography.X509Certificates.RSACertificateExtensions]::CopyWithPrivateKey($leaf, $leafKey).Export(
  [Security.Cryptography.X509Certificates.X509ContentType]::Pfx, 'gesher')
$serverCert = [Security.Cryptography.X509Certificates.X509Certificate2]::new($leafPfx, 'gesher',
  [Security.Cryptography.X509Certificates.X509KeyStorageFlags]::Exportable)
$rootPublic = [Security.Cryptography.X509Certificates.X509Certificate2]::new($rootCert.Export(
  [Security.Cryptography.X509Certificates.X509ContentType]::Cert))

if (-not ('GesherNetTlsServer' -as [type])) { Add-Type -Path "$PSScriptRoot/../net/TlsServer.cs" }
[GesherNetTlsServerFixture]::Test($serverCert)
Write-Host 'PASS  net fixture: idle accept, incomplete TLS handshake and accepted idle HTTP read stop with no surviving clients'
$tlsServer = [GesherNetTlsServer]::new($serverCert)
$machineRoot = [Security.Cryptography.X509Certificates.X509Store]::new('Root', 'LocalMachine')
$url = "https://localhost:$($tlsServer.Port)/"
try {
  $null = NetDex 'a test root not in Windows'' store is not trusted' @('https', $url, 'reject') 180
  $machineRoot.Open('ReadWrite')
  $machineRoot.Add($rootPublic)
  $machineRoot.Close()
  $out = NetDex 'a test root added to Windows'' machine ROOT store is trusted' @('https', $url, 'ok') 180
  # The root is in the system CA directory, under its subject hash.
  $found = @(Get-ChildItem $cacerts | Where-Object { (Get-Content -Raw $_.FullName).Contains([Convert]::ToBase64String($rootPublic.RawData).Substring(0, 64)) })
  if ($found.Count -eq 1) {
    Write-Host "PASS  net: the Windows root is in `$ANDROID_ROOT/etc/security/cacerts as $($found[0].Name)"
  } else {
    Write-Host "FAIL  net: the Windows root is in `$ANDROID_ROOT/etc/security/cacerts $($found.Count) times"
    $failed++
  }
  $null = NetDex 'GESHER_WINDOWS_CERTS=0: only Android''s CAs, the Windows root is not trusted' @('https', $url, 'reject') 180 @{ GESHER_WINDOWS_CERTS = '0' }
  $machineRoot.Open('ReadWrite')
  $machineRoot.Remove($rootPublic)
  $machineRoot.Close()
  $null = NetDex 'the test root removed from Windows'' store is not trusted any more' @('https', $url, 'reject') 180
} finally {
  Write-Host 'INFO  net TLS cleanup: root-test server stop'
  try { $tlsServer.Dispose() } finally {
    Write-Host 'INFO  net TLS cleanup: root-test certificate store remove'
    try { $machineRoot.Open('ReadWrite'); $machineRoot.Remove($rootPublic); $machineRoot.Close() } catch { }
  }
  Write-Host 'INFO  net TLS cleanup: root-test complete'
}

# On the internet (the runners have it): DNS, HTTPS GETs with TLS 1.3 and SNI, and
# certificates that must be rejected (expired, another host's, self-signed, an
# untrusted root).
$null = NetDex 'DNS and HTTPS on the internet, bad certificates rejected' @('internet') 300

# Apps (tests/apps/netapp, nonetapp) through gesher-app: ConnectivityManager over the
# connectivity service (com.gesher.server.ConnectivityService), its callbacks, HTTPS with
# the network security config, and the permissions: INTERNET and ACCESS_NETWORK_STATE.
$bcpNet = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$dvmNet = @("-Xbootclasspath:$bcpNet", "-Ximage:$(NoDrive "$root/system")/framework/boot.art", '-Xint', '-Xusejit:false')

# Runs dalvikvm with a time limit; returns its output, $LASTEXITCODE its exit code (-1: timed out).
function NetRun([string[]] $Arguments, [int] $Seconds = 300) {
  $psi = [Diagnostics.ProcessStartInfo]::new("$Dir/dalvikvm.exe")
  foreach ($a in $Arguments) { $psi.ArgumentList.Add($a) }
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $p = [Diagnostics.Process]::Start($psi)
  $stdout = $p.StandardOutput.ReadToEndAsync()
  $stderr = $p.StandardError.ReadToEndAsync()
  $timedOut = -not $p.WaitForExit($Seconds * 1000)
  if ($timedOut) { $p.Kill($true); $p.WaitForExit() }
  $null = $stdout.Wait(10000), $stderr.Wait(10000)
  $global:LASTEXITCODE = if ($timedOut) { -1 } else { $p.ExitCode }
  $out = $stdout.Result + "`n" + $stderr.Result
  if ($timedOut) { $out += "`n(timed out after $Seconds s)" }
  return $out
}

function NetExpect([string] $Name, [string] $Out, [int] $Code, [string[]] $Expect, [string] $Show = '^(NetApp|NoNetApp) ') {
  $missing = @($Expect | Where-Object { -not $Out.Contains($_) })
  if ($Code -eq 0 -and -not $missing) {
    Write-Host "PASS  net: $Name"
  } else {
    Write-Host "FAIL  net: $Name (exit $Code; missing: $($missing -join ' | '))"
    ($Out -split "`n" | Select-Object -Last 80) | Out-Host
    $script:failed++
  }
  ($Out -split "`n" | Select-String -Pattern $Show | ForEach-Object { "INFO  $_".TrimEnd() }) | Out-Host
}

$out = NetRun ($dvmNet + 'com.gesher.app.Main', (NoDrive "$Dir/netapp.apk")) 400
NetExpect 'netapp.apk in one process: ConnectivityManager (Windows'' active network, its capabilities, link properties and NetworkInfo), a default and a request callback, HTTPS through HttpsURLConnection, cleartext refused (target SDK 36)' $out $LASTEXITCODE @(
  'NetApp state: internet true', 'connected true', 'NetApp default callback: available true',
  'NetApp request callback (INTERNET): available true', 'NetApp https: 200 Example Domain',
  'NetApp cleartext http: refused', 'NetApp done', 'gesher-app: done')

$out = NetRun ($dvmNet + 'com.gesher.app.Main', (NoDrive "$Dir/nonetapp.apk")) 300
NetExpect 'nonetapp.apk without INTERNET and ACCESS_NETWORK_STATE: name lookups and sockets refused (EACCES), ConnectivityManager throws SecurityException' $out $LASTEXITCODE @(
  'NoNetApp dns: SecurityException: Permission denied (missing INTERNET permission?)',
  'NoNetApp socket: SocketException', 'EACCES',
  'NoNetApp connectivity: SecurityException', 'NoNetApp done', 'gesher-app: done')

# Across processes: a system server (with the connectivity service) and netapp.apk in an
# app process attached to it. The app waits in a default network callback while this
# script adds an address to the adapter of Windows' default route and removes it again:
# Windows' change reaches the app as onLinkPropertiesChanged both times. The address is
# IPv6 (2001:db8::/32, documentation), never IPv4: a static IPv4 address on a DHCP
# adapter turns its DHCP off, which cut the runners off GitHub (ci 40133972018).
$device = "net-$PID"
$netLogs = Join-Path $root 'net-logs'
New-Item -ItemType Directory -Force $netLogs | Out-Null
$env:GESHER_BINDER = $device
$started = @()
$started += Start-Process "$Dir/binderd.exe" -ArgumentList $device -PassThru -NoNewWindow `
  -RedirectStandardOutput "$netLogs/binderd.out" -RedirectStandardError "$netLogs/binderd.log"
$pipe = "\\.\pipe\gesher-binder-$device"
for ($i = 0; $i -lt 150 -and -not ([IO.Directory]::GetFiles('\\.\pipe\') -contains $pipe); $i++) { Start-Sleep -Milliseconds 100 }
$started += Start-Process "$Dir/servicemanager.exe" -PassThru -NoNewWindow `
  -RedirectStandardOutput "$netLogs/servicemanager.out" -RedirectStandardError "$netLogs/servicemanager.log"
$server = Start-Process "$Dir/dalvikvm.exe" -PassThru -NoNewWindow `
  -ArgumentList ($dvmNet + '-Dgesher.net.trace=true', 'com.gesher.server.SystemServer', (NoDrive "$Dir/netapp.apk"), (NoDrive "$Dir/nonetapp.apk")) `
  -RedirectStandardOutput "$netLogs/system_server.out" -RedirectStandardError "$netLogs/system_server.err"
$started += $server
for ($i = 0; $i -lt 900 -and -not $server.HasExited; $i++) {
  if ("$(Get-Content -Raw "$netLogs/system_server.out" -ErrorAction SilentlyContinue)".Contains('with servicemanager')) { break }
  Start-Sleep -Milliseconds 100
}
$route = Get-NetRoute -DestinationPrefix '0.0.0.0/0' -ErrorAction SilentlyContinue | Sort-Object RouteMetric, InterfaceMetric | Select-Object -First 1
$appLog = Join-Path $netLogs 'netapp.out'
$appArgs = ($dvmNet + 'com.gesher.app.Main', '--system-server', '--ez', 'watch', 'true', (NoDrive "$Dir/netapp.apk")) -join ' '
$psi = [Diagnostics.ProcessStartInfo]::new('cmd.exe', "/d /s /c `"`"$Dir/dalvikvm.exe`" $appArgs > `"$appLog`" 2>&1`"")
$psi.UseShellExecute = $false
$app = [Diagnostics.Process]::Start($psi)
$added = $false
$dhcp4 = $null
$appCode = -1
try {
  $deadline = (Get-Date).AddSeconds(400)
  while ((Get-Date) -lt $deadline -and -not $app.HasExited -and
         -not "$(Get-Content -Raw $appLog -ErrorAction SilentlyContinue)".Contains('watching: add')) { Start-Sleep -Milliseconds 250 }
  if ($route -and -not $app.HasExited) {
    $dhcp4 = (Get-NetIPInterface -InterfaceIndex $route.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue).Dhcp
    $null = New-NetIPAddress -InterfaceIndex $route.ifIndex -AddressFamily IPv6 -IPAddress 2001:db8::77 -PrefixLength 128 `
      -SkipAsSource $true -PolicyStore ActiveStore -ErrorAction Stop
    $added = $true
    $deadline = (Get-Date).AddSeconds(100)
    while ((Get-Date) -lt $deadline -and -not $app.HasExited -and
           -not "$(Get-Content -Raw $appLog -ErrorAction SilentlyContinue)".Contains('watch: added seen')) { Start-Sleep -Milliseconds 250 }
    Remove-NetIPAddress -InterfaceIndex $route.ifIndex -IPAddress 2001:db8::77 -Confirm:$false -ErrorAction SilentlyContinue
    $added = $false
  }
  if (-not $app.WaitForExit(200000)) { $app.Kill($true) }
  if ($app.HasExited) { $appCode = $app.ExitCode }
} finally {
  if ($added) { Remove-NetIPAddress -InterfaceIndex $route.ifIndex -IPAddress 2001:db8::77 -Confirm:$false -ErrorAction SilentlyContinue }
  # The runner's IPv4 must be as it was (DHCP), whatever happened above.
  if ($route -and $dhcp4 -eq 'Enabled' -and
      (Get-NetIPInterface -InterfaceIndex $route.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue).Dhcp -ne 'Enabled') {
    Write-Host "INFO  net: DHCPv4 was turned off on interface $($route.ifIndex); turning it on again"
    Set-NetIPInterface -InterfaceIndex $route.ifIndex -AddressFamily IPv4 -Dhcp Enabled -ErrorAction SilentlyContinue
  }
  foreach ($p in $started) { if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue } }
  Remove-Item Env:GESHER_BINDER
}
$out = "$(Get-Content -Raw $appLog -ErrorAction SilentlyContinue)"
if (-not $route) { $out += "`n(no default route on this machine)" }
$failedBefore = $failed
NetExpect "netapp.apk in an app process of a system server: ConnectivityManager over binder; an address added to and removed from the adapter of Windows' default route (interface $($route.ifIndex)) reaches its default network callback" $out $appCode @(
  'NetApp state: internet true', 'NetApp default callback: available true', 'NetApp https: 200 Example Domain',
  'NetApp watch: added seen true', 'NetApp watch: removed seen true', 'NetApp done', 'gesher-app: done')
if ($failed -ne $failedBefore) {
  Get-Content "$netLogs/system_server.out", "$netLogs/system_server.err" -ErrorAction SilentlyContinue | Select-Object -Last 60 | Out-Host
}
