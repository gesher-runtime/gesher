# Stage verified, unmodified microG addons in a Gesher profile. Google Check-in,
# Cloud Messaging and account setup are controlled by microG's own settings.
param(
    [string] $GmsCoreApk,
    [string] $CompanionApk,
    [string] $GsfProxyApk,
    [string] $StoreApk,
    [switch] $AutoDownload,
    [switch] $InstallStore,
    [switch] $StoreOnly,
    [string] $ProfileRoot = (Join-Path $env:LOCALAPPDATA 'Gesher'),
    [string] $RuntimeDir = $PSScriptRoot,
    [switch] $Disable,
    [switch] $Remove
)
$ErrorActionPreference = 'Stop'
$scriptRoot = if ($PSScriptRoot) { $PSScriptRoot } elseif ($MyInvocation.MyCommand.Path) { Split-Path -Parent $MyInvocation.MyCommand.Path } else { (Get-Location).Path }
. (Join-Path $scriptRoot 'gesher-ui.ps1')
$ProfileRoot = [IO.Path]::GetFullPath($ProfileRoot)
$RuntimeDir = if ($RuntimeDir) { [IO.Path]::GetFullPath($RuntimeDir) } else { $scriptRoot }
$marker = Join-Path $ProfileRoot 'microg.enabled'
$packages = 'com.google.android.gms', 'com.android.vending', 'com.google.android.gsf'
function Initialize-MicrogDefaults([string] $ProfileRoot) {
    # Pre-configure microG settings (Checkin & GCM enabled out of the box)
    $prefsDir = Join-Path $ProfileRoot 'data/data/com.google.android.gms/shared_prefs'
    New-Item -ItemType Directory -Force -Path $prefsDir | Out-Null

    $checkinXml = @"
<?xml version='1.0' encoding='utf-8' standalone='yes' ?>
<map>
    <boolean name="checkin_enable_service" value="true" />
</map>
"@
    if (-not (Test-Path -LiteralPath (Join-Path $prefsDir 'checkin.xml'))) {
    Set-Content -LiteralPath (Join-Path $prefsDir 'checkin.xml') -Value $checkinXml -Encoding UTF8
    }

    $gcmXml = @"
<?xml version='1.0' encoding='utf-8' standalone='yes' ?>
<map>
    <boolean name="gcm_enable_mcs_service" value="true" />
</map>
"@
    if (-not (Test-Path -LiteralPath (Join-Path $prefsDir 'gcm.xml'))) {
    Set-Content -LiteralPath (Join-Path $prefsDir 'gcm.xml') -Value $gcmXml -Encoding UTF8
    }

    $settingsXml = @"
<?xml version='1.0' encoding='utf-8' standalone='yes' ?>
<map>
    <boolean name="checkin_enable_service" value="true" />
    <boolean name="gcm_enable_mcs_service" value="true" />
</map>
"@
    if (-not (Test-Path -LiteralPath (Join-Path $prefsDir 'org.microg.gms.settings_preferences.xml'))) {
    Set-Content -LiteralPath (Join-Path $prefsDir 'org.microg.gms.settings_preferences.xml') -Value $settingsXml -Encoding UTF8
    }

    # Pre-seed runtime permissions for official microG
    $permFile = Join-Path $ProfileRoot 'data/system/runtime-permissions'
    $permLines = @()
    if (Test-Path -LiteralPath $permFile) {
        $permLines = @(Get-Content -LiteralPath $permFile)
    }
    $signer = '9bd06727e62796c0130eb6dab39b73157451582cbd138e86c468acc395d14165'
    foreach ($permission in 'ACCESS_FINE_LOCATION', 'ACCESS_COARSE_LOCATION', 'POST_NOTIFICATIONS') {
        # Java Properties treats an unescaped colon as a key/value separator.
        $key = "com.google.android.gms\:$signer\:android.permission.$permission"
        if (-not @($permLines | Where-Object { $_.StartsWith($key + '=') }).Count) {
            $permLines += "$key=granted"
        }
    }
    New-Item -ItemType Directory -Force -Path (Split-Path $permFile) | Out-Null
    Set-Content -LiteralPath $permFile -Value $permLines -Encoding ASCII
}
function Install-AuroraStore {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        $storeTarget = if ($StoreApk -and (Test-Path -LiteralPath $StoreApk)) {
            $StoreApk
        } else {
            $tmp = Join-Path $env:TEMP "AuroraStore.apk"
            if (-not (Test-Path $tmp)) {
                Write-Host "Downloading Aurora Store..."
                Invoke-WebRequest -Uri "https://auroraoss.com/downloads/AuroraStore/Release/AuroraStore-4.6.1.apk" -OutFile $tmp -UseBasicParsing
            }
            $tmp
        }
        Write-Host "Installing Aurora Store..."
        & (Join-Path $RuntimeDir "gesher-app.ps1") install "$storeTarget"
    }
if ($Disable -and $Remove) { throw (Get-GesherText 'microg_choose') }
if (($Disable -or $Remove) -and ($GmsCoreApk -or $CompanionApk -or $GsfProxyApk -or
        $StoreApk -or $AutoDownload -or $InstallStore -or $StoreOnly)) {
    throw (Get-GesherText 'microg_no_files')
}
if ($StoreOnly -or ($InstallStore -and -not $AutoDownload -and -not $GmsCoreApk -and
        -not $CompanionApk -and -not $GsfProxyApk)) { Install-AuroraStore; return }
if ($Disable) {
    if (Test-Path -LiteralPath $marker) { Remove-Item -LiteralPath $marker }
    Write-Host (Get-GesherText 'microg_disabled')
    return
}
# Replacing mapped APK contents can invalidate the verified identity. Require the
# device to be stopped, and stage complete verified files before changing slots.
$ready = Join-Path $ProfileRoot 'data/system/gesher-device.ready'
if (Test-Path -LiteralPath $ready) {
    $serverId = 0
    if ([int]::TryParse((Get-Content -LiteralPath $ready -Raw).Trim(), [ref] $serverId)) {
        if (Get-Process -Id $serverId -ErrorAction SilentlyContinue) {
            throw (Get-GesherText 'microg_stop')
        }
    }
}
if ($Remove) {
    if (Test-Path -LiteralPath $marker) { Remove-Item -LiteralPath $marker }
    foreach ($package in $packages) {
        # Only known filenames; never recursively remove application/account data.
        foreach ($name in 'base.apk', 'base.apk.new') {
            $file = Join-Path $ProfileRoot "system/priv-app/$package/$name"
            if (Test-Path -LiteralPath $file) { Remove-Item -LiteralPath $file }
        }
    }
    Write-Host (Get-GesherText 'microg_removed')
    return
}
if ($AutoDownload) {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $downloadDir = Join-Path $env:TEMP "gesher-microg-downloads"
    New-Item -ItemType Directory -Force -Path $downloadDir | Out-Null
    if (-not $GmsCoreApk) {
        $gmsTarget = Join-Path $downloadDir 'com.google.android.gms.apk'
        if (-not (Test-Path $gmsTarget)) {
            Write-Host "Downloading official microG Services (GmsCore)..."
            Invoke-WebRequest -Uri "https://github.com/microg/GmsCore/releases/download/v0.3.17.252432/com.google.android.gms-252432034.apk" -OutFile $gmsTarget -UseBasicParsing
        }
        $GmsCoreApk = $gmsTarget
    }
    if (-not $CompanionApk) {
        $vendingTarget = Join-Path $downloadDir 'com.android.vending.apk'
        if (-not (Test-Path $vendingTarget)) {
            Write-Host "Downloading official microG Companion..."
            Invoke-WebRequest -Uri "https://github.com/microg/GmsCore/releases/download/v0.3.17.252432/com.android.vending-84022634.apk" -OutFile $vendingTarget -UseBasicParsing
        }
        $CompanionApk = $vendingTarget
    }
    if (-not $GsfProxyApk) {
        $gsfTarget = Join-Path $downloadDir 'com.google.android.gsf.apk'
        if (-not (Test-Path $gsfTarget)) {
            Write-Host "Downloading official GsfProxy..."
            Invoke-WebRequest -Uri "https://github.com/microg/GsfProxy/releases/download/v0.1.0/GsfProxy.apk" -OutFile $gsfTarget -UseBasicParsing
        }
        $GsfProxyApk = $gsfTarget
    }
}
if (-not $GmsCoreApk) { throw (Get-GesherText 'microg_pass_apk') }
$entries = @(
    @{ Package = $packages[0]; Source = $GmsCoreApk },
    @{ Package = $packages[1]; Source = $CompanionApk },
    @{ Package = $packages[2]; Source = $GsfProxyApk }
) | Where-Object { $_.Source }
foreach ($entry in $entries) {
    if (-not (Test-Path -LiteralPath $entry.Source -PathType Leaf)) {
        throw (Get-GesherText 'microg_missing' @($entry.Source))
    }
    $entry.Source = (Resolve-Path -LiteralPath $entry.Source).ProviderPath
}
$runtimeDrive = [IO.Path]::GetPathRoot($RuntimeDir).TrimEnd('\').TrimEnd(':')
function AndroidPath([string] $path) {
    $full = [IO.Path]::GetFullPath($path) -replace '\\', '/'
    if ($full -match '^([A-Za-z]):(/.*)$') {
        if ($Matches[1] -ieq $runtimeDrive) { return $Matches[2] }
        return "/mnt/$($Matches[1].ToLower())$($Matches[2])"
    }
    return $full
}
$dalvikvm = Join-Path $RuntimeDir 'dalvikvm.exe'
if (-not (Test-Path -LiteralPath $dalvikvm)) {
    throw (Get-GesherText 'microg_runtime')
}
# Only public runtime data needed to start the offline APK verifier. Child-only
# environment variables keep the calling shell's profile/UID unchanged.
foreach ($dir in 'system', 'art', 'data', 'i18n/etc/icu') {
    New-Item -ItemType Directory -Force (Join-Path $ProfileRoot $dir) | Out-Null
}
$icu = @(Get-ChildItem -LiteralPath $RuntimeDir -Filter 'icudt*l.dat')
if ($icu.Count -ne 1) { throw (Get-GesherText 'microg_icu') }
$icuTarget = Join-Path $ProfileRoot "i18n/etc/icu/$($icu[0].Name)"
if (-not (Test-Path -LiteralPath $icuTarget) -or (Get-Item $icuTarget).Length -ne $icu[0].Length) {
    Copy-Item -LiteralPath $icu[0].FullName -Destination $icuTarget -Force
}
$bcp = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') |
    ForEach-Object { AndroidPath (Join-Path $RuntimeDir "$_.jar") }) -join ':'
$staged = @()
try {
    foreach ($entry in $entries) {
        $slot = Join-Path $ProfileRoot "system/priv-app/$($entry.Package)"
        New-Item -ItemType Directory -Force -Path $slot | Out-Null
        $temp = Join-Path $slot 'base.apk.new'
        Copy-Item -LiteralPath $entry.Source -Destination $temp -Force
        $staged += $temp
        $arguments = @("-Xbootclasspath:$bcp", "-Ximage:$(AndroidPath (Join-Path $RuntimeDir 'framework/boot.art'))",
            '-Xint', '-Xusejit:false', 'com.gesher.server.MicrogAddons',
            'verify', $entry.Package, (AndroidPath $temp))
        $psi = [Diagnostics.ProcessStartInfo]::new($dalvikvm)
        $psi.Arguments = ($arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
        $psi.WorkingDirectory = $RuntimeDir
        $psi.UseShellExecute = $false
        $psi.CreateNoWindow = $true
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        $psi.EnvironmentVariables['ANDROID_ROOT'] = AndroidPath "$ProfileRoot/system"
        $psi.EnvironmentVariables['ANDROID_ART_ROOT'] = AndroidPath "$ProfileRoot/art"
        $psi.EnvironmentVariables['ANDROID_DATA'] = AndroidPath "$ProfileRoot/data"
        $psi.EnvironmentVariables['ANDROID_I18N_ROOT'] = AndroidPath "$ProfileRoot/i18n"
        $psi.EnvironmentVariables['ANDROID_TZDATA_ROOT'] = AndroidPath "$RuntimeDir/tzdata"
        $process = [Diagnostics.Process]::Start($psi)
        try {
            $output = $process.StandardOutput.ReadToEndAsync()
            $errors = $process.StandardError.ReadToEndAsync()
            if (-not $process.WaitForExit(120000)) {
                $process.Kill()
                $process.WaitForExit()
                throw (Get-GesherText 'microg_timeout')
            }
            $stdout = $output.GetAwaiter().GetResult()
            $stderr = $errors.GetAwaiter().GetResult()
            if ($process.ExitCode -ne 0 -or -not $stdout.Contains("Verified official microG addon: $($entry.Package)")) {
                throw (Get-GesherText 'microg_verify_failed' @($entry.Package, $stderr))
            }
            Write-Host (Get-GesherText 'microg_verified' @($entry.Package))
        } finally { $process.Dispose() }
    }
    foreach ($entry in $entries) {
        $slot = Join-Path $ProfileRoot "system/priv-app/$($entry.Package)"
        Move-Item -LiteralPath (Join-Path $slot 'base.apk.new') -Destination (Join-Path $slot 'base.apk') -Force
    }
    Set-Content -LiteralPath $marker -Value 'official-microg-v1' -Encoding ASCII

    Initialize-MicrogDefaults $ProfileRoot



} finally {
    foreach ($temp in $staged) {
        if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp }
    }
}
if ($InstallStore) { Install-AuroraStore }
Write-Host (Get-GesherText 'microg_staged')
Write-Host (Get-GesherText 'microg_settings')
