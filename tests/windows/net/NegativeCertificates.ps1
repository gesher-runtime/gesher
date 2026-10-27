# Local certificate fixtures for Conscrypt's rejection tests. The caller
# trusts only $Issuer in the existing test-root lifetime and owns the results.
function New-NetTestCertificate([string] $HostName, $Issuer, [DateTimeOffset] $From,
                                [DateTimeOffset] $Until, [bool] $SelfSigned = $false) {
  $key = [Security.Cryptography.RSA]::Create(2048)
  try {
    $request = [Security.Cryptography.X509Certificates.CertificateRequest]::new(
      "CN=$HostName", $key, [Security.Cryptography.HashAlgorithmName]::SHA256,
      [Security.Cryptography.RSASignaturePadding]::Pkcs1)
    $san = [Security.Cryptography.X509Certificates.SubjectAlternativeNameBuilder]::new()
    $san.AddDnsName($HostName)
    $request.CertificateExtensions.Add($san.Build())
    $eku = [Security.Cryptography.OidCollection]::new()
    $null = $eku.Add([Security.Cryptography.Oid]::new('1.3.6.1.5.5.7.3.1'))
    $request.CertificateExtensions.Add([Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]::new($eku, $false))
    $request.CertificateExtensions.Add([Security.Cryptography.X509Certificates.X509BasicConstraintsExtension]::new($false, $false, 0, $true))
    $serial = [byte[]]::new(16)
    [Security.Cryptography.RandomNumberGenerator]::Fill($serial)
    $serial[0] = 0x42
    if ($SelfSigned) {
      $certificate = $request.CreateSelfSigned($From, $Until)
    } else {
      $public = $request.Create($Issuer, $From, $Until, $serial)
      $certificate = [Security.Cryptography.X509Certificates.RSACertificateExtensions]::CopyWithPrivateKey($public, $key)
      $public.Dispose()
    }
    try {
      $pfx = $certificate.Export([Security.Cryptography.X509Certificates.X509ContentType]::Pfx, 'gesher')
      return [Security.Cryptography.X509Certificates.X509Certificate2]::new($pfx, 'gesher',
        [Security.Cryptography.X509Certificates.X509KeyStorageFlags]::Exportable)
    } finally { $certificate.Dispose() }
  } finally { $key.Dispose() }
}
