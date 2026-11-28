param([Parameter(Mandatory)][string] $RuntimeDir)
$ErrorActionPreference = 'Stop'
$RuntimeDir = (Resolve-Path -LiteralPath $RuntimeDir).Path
function AndroidPath([string] $Path) { ($Path -replace '\\', '/') -replace '^[A-Za-z]:', '' }
$root = Join-Path $env:TEMP "g56-provider-$([guid]::NewGuid().ToString('N').Substring(0, 8))"
foreach ($name in 'system/framework', 'art', 'data', 'i18n/etc/icu', 'tzdata') {
    New-Item -ItemType Directory -Force (Join-Path $root $name) | Out-Null
}
try {
    Copy-Item "$RuntimeDir/icudt*l.dat" "$root/i18n/etc/icu"
    Copy-Item "$RuntimeDir/framework-res.apk" "$root/system/framework"
    Copy-Item "$RuntimeDir/build.prop" "$root/system"
    Copy-Item -Recurse "$RuntimeDir/tzdata/*" "$root/tzdata"
    $env:ANDROID_ROOT = AndroidPath "$root/system"
    $env:ANDROID_ART_ROOT = AndroidPath "$root/art"
    $env:ANDROID_DATA = AndroidPath "$root/data"
    $env:ANDROID_I18N_ROOT = AndroidPath "$root/i18n"
    $env:ANDROID_TZDATA_ROOT = AndroidPath "$root/tzdata"
    Remove-Item Env:GESHER_REQUIRE_BOOT_IMAGE -ErrorAction SilentlyContinue
    $dex = Join-Path $PSScriptRoot 'dex/classes.dex'
    $libraries = 'android.test.base.jar', 'org.apache.http.legacy.jar'
    $boot = 'core-oj.jar', 'core-libart.jar', 'core-icu4j.jar', 'framework.jar', 'gesher-app.jar'
    foreach ($file in @($dex) + (($boot + $libraries) | ForEach-Object { Join-Path $RuntimeDir $_ })) {
        Write-Host "PROVIDER_INPUT $([IO.Path]::GetFileName($file)) SHA256=$((Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash)"
    }
    $bcp = ($boot | ForEach-Object { AndroidPath "$RuntimeDir/$_" }) -join ':'
    & "$RuntimeDir/dalvikvm.exe" "-Xbootclasspath:$bcp" "-Ximage:$(AndroidPath "$root/absent.art")" `
        '-Xint' '-Xusejit:false' '-cp' (AndroidPath $dex) 'gesher.ProviderContracts' `
        (AndroidPath "$RuntimeDir/$($libraries[0])") (AndroidPath "$RuntimeDir/$($libraries[1])")
    if ($LASTEXITCODE -ne 0) { throw "ProviderContracts exited $LASTEXITCODE" }
} finally {
    Remove-Item -Recurse -Force -LiteralPath $root
}
