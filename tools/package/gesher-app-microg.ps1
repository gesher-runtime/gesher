# PROTOTYPE: Packaging & Launcher extension for optional microG support in Gesher
# Extends tools/package/gesher-app.ps1 to stage microG priv-app APKs and privapp-permissions XML.

param(
    [switch] $EnableMicroG = $false,
    [string] $MicroGDir = ""
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent (Split-Path -Parent $here)

function NoDrive([string] $Path) {
    return ($Path -replace '^[A-Za-z]:', '') -replace '\\', '/'
}

$env:ANDROID_ROOT = NoDrive "$root/system"
$env:ANDROID_ART_ROOT = NoDrive "$root/art"
$env:ANDROID_DATA = NoDrive "$root/data"
$env:ANDROID_I18N_ROOT = NoDrive "$root/i18n"
$env:ANDROID_TZDATA_ROOT = NoDrive "$here/tzdata"
$env:GESHER_BINDER = "gesher-app-$PID"

# Stage system/priv-app and permissions layout for microG when enabled
$privAppDir = Join-Path $root "system/priv-app"
$permDir = Join-Path $root "system/etc/permissions"
New-Item -ItemType Directory -Force -Path $privAppDir, $permDir | Out-Null

$microgApks = @()
if ($EnableMicroG) {
    $env:GESHER_MICROG_SPOOF = "1"
    $permXmlSource = Join-Path $here "privapp-permissions-microg.xml"
    if (Test-Path $permXmlSource) {
        Copy-Item $permXmlSource (Join-Path $permDir "privapp-permissions-microg.xml") -Force
    }

    $candidates = @(
        @{ Name = "GmsCore";   Pkg = "com.google.android.gms"; File = "com.google.android.gms.apk" },
        @{ Name = "FakeStore"; Pkg = "com.android.vending";    File = "com.android.vending.apk" },
        @{ Name = "GsfProxy";  Pkg = "com.google.android.gsf"; File = "com.google.android.gsf.apk" }
    )

    foreach ($c in $candidates) {
        $targetDir = Join-Path $privAppDir $c.Name
        New-Item -ItemType Directory -Force -Path $targetDir | Out-Null
        $destApk = Join-Path $targetDir $c.File
        if ($MicroGDir -and (Test-Path (Join-Path $MicroGDir $c.File))) {
            Copy-Item (Join-Path $MicroGDir $c.File) $destApk -Force
        }
        if (Test-Path $destApk) {
            $microgApks += (NoDrive $destApk)
        }
    }
}
