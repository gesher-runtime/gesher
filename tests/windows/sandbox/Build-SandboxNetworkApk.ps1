# Builds the isolated Android network fixture with operator-selected endpoints.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $AndroidSdk,
    [Parameter(Mandatory)][string] $OutputDir,
    [Parameter(Mandatory)][uri] $PublicUrl,
    [Parameter(Mandatory)][string] $PublicMarker,
    [Parameter(Mandatory)][uri] $HostUrl,
    [Parameter(Mandatory)][string] $HostMarker,
    [uri] $LoopbackUrl,
    [uri] $LanUrl,
    [string] $LanMarker = 'Gesher-LAN-Probe'
)

$ErrorActionPreference = 'Stop'
$project = (Resolve-Path (Join-Path $PSScriptRoot '../../apps/sandboxnetwork')).Path
$sdk = (Resolve-Path -LiteralPath $AndroidSdk).Path
$tools = Join-Path $sdk 'build-tools/36.0.0'
$android = Join-Path $sdk 'platforms/android-36/android.jar'
$out = (New-Item -ItemType Directory -Force -Path $OutputDir).FullName
$assets = (New-Item -ItemType Directory -Force -Path (Join-Path $out 'assets')).FullName
$classes = (New-Item -ItemType Directory -Force -Path (Join-Path $out 'classes')).FullName
$dex = (New-Item -ItemType Directory -Force -Path (Join-Path $out 'dex')).FullName

function Endpoint([string] $Group, [uri] $Url, [string] $Marker) {
    if ($Url.Scheme -notin 'http','https' -or $Url.UserInfo -or $Marker -notmatch '^[A-Za-z0-9 ._-]+$') {
        throw "Use an HTTP(S) URL without credentials and a printable content marker for $Group"
    }
    return "$Group`t$($Url.AbsoluteUri)`t$Marker"
}

$config = @(Endpoint 'public' $PublicUrl $PublicMarker; Endpoint 'host' $HostUrl $HostMarker)
if ($LoopbackUrl) { $config += Endpoint 'loopback' $LoopbackUrl $HostMarker }
if ($LanUrl) { $config += Endpoint 'lan' $LanUrl $LanMarker }
[IO.File]::WriteAllLines((Join-Path $assets 'endpoints.tsv'), $config, [Text.Encoding]::ASCII)

function Invoke-Checked([string] $Program, [string[]] $Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program exited $LASTEXITCODE" }
}

$source = Join-Path $project 'java/com/gesher/sandboxnetwork/ProbeActivity.java'
Invoke-Checked 'javac' @('--release','17','-classpath',$android,'-d',$classes,$source)
$classFiles = @(Get-ChildItem -Recurse -LiteralPath $classes -Filter '*.class' | ForEach-Object FullName)
Invoke-Checked (Join-Path $tools 'd8.bat') (@('--min-api','28','--lib',$android,'--output',$dex) + $classFiles)
$unsigned = Join-Path $out 'sandbox-network-unsigned.apk'
$apk = Join-Path $out 'sandbox-network.apk'
Invoke-Checked (Join-Path $tools 'aapt2.exe') @('link','-I',$android,'--manifest',
    (Join-Path $project 'AndroidManifest.xml'),'-A',$assets,'--min-sdk-version','28','--target-sdk-version','32','-o',$unsigned)
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::Open($unsigned, [IO.Compression.ZipArchiveMode]::Update)
try {
    [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
        $archive, (Join-Path $dex 'classes.dex'), 'classes.dex')
} finally { $archive.Dispose() }

$key = Join-Path $out 'sandbox-test.jks'
if (-not (Test-Path -LiteralPath $key)) {
    Invoke-Checked 'keytool' @('-genkeypair','-keystore',$key,'-storepass','android','-keypass','android',
        '-alias','sandbox-network-fixture','-keyalg','RSA','-dname','CN=Gesher sandbox network fixture','-validity','365')
}
Invoke-Checked (Join-Path $tools 'apksigner.bat') @('sign','--ks',$key,'--ks-pass','pass:android','--out',$apk,$unsigned)
Invoke-Checked (Join-Path $tools 'apksigner.bat') @('verify',$apk)
Get-Item -LiteralPath $apk
