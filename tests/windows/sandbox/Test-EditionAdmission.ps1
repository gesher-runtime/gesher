# Exercises the packaged Windows launcher and system server with signed APKs.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $RuntimeDir,
    [Parameter(Mandatory)][string] $AndroidSdk,
    [Parameter(Mandatory)][string] $OutputDir
)

$ErrorActionPreference = 'Stop'
$runtime = (Resolve-Path -LiteralPath $RuntimeDir).Path
$project = Join-Path $PSScriptRoot '../../apps/editionadmission'
$tools = Join-Path $AndroidSdk 'build-tools/36.0.0'
$android = Join-Path $AndroidSdk 'platforms/android-36/android.jar'
$runDir = Join-Path $OutputDir ('admission-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
$out = (New-Item -ItemType Directory -Path $runDir -Force).FullName
$classes = (New-Item -ItemType Directory -Path (Join-Path $out 'classes') -Force).FullName
$dex = (New-Item -ItemType Directory -Path (Join-Path $out 'dex') -Force).FullName
$packagePrefix = 'com.gesher.qa.admission'

function Invoke-Checked([string] $Program, [string[]] $Arguments) {
    & $Program @Arguments | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "$Program exited $LASTEXITCODE" }
}

Invoke-Checked 'javac' @('--release', '17', '-classpath', $android, '-d', $classes, (Join-Path $project 'MainActivity.java'))
Invoke-Checked (Join-Path $tools 'd8.bat') @('--min-api', '28', '--lib', $android, '--output', $dex,
    (Join-Path $classes 'com/gesher/qa/admission/MainActivity.class'))
$key = Join-Path $out 'edition-fixture.jks'
Invoke-Checked 'keytool' @('-genkeypair', '-keystore', $key, '-storepass', 'android', '-keypass', 'android',
    '-alias', 'edition-fixture', '-keyalg', 'RSA', '-dname', 'CN=Gesher admission fixture', '-validity', '365')

function Build-Apk([string] $Name, [string] $Package, [string] $Feature, [int] $Version) {
    $manifest = Join-Path $out "$Name.xml"
    $unsigned = Join-Path $out "$Name-unsigned.apk"
    $apk = Join-Path $out "$Name.apk"
    [IO.File]::WriteAllText($manifest, @"
<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="$Package" android:versionCode="$Version">
    $Feature
    <uses-permission android:name="android.permission.ACCESS_COARSE_LOCATION"/>
    <application android:label="Gesher admission fixture">
        <activity android:name="$packagePrefix.MainActivity" android:exported="true">
            <intent-filter>
                <action android:name="android.intent.action.MAIN"/>
                <category android:name="android.intent.category.LAUNCHER"/>
            </intent-filter>
            <intent-filter>
                <action android:name="android.intent.action.VIEW"/>
                <category android:name="android.intent.category.DEFAULT"/>
                <category android:name="android.intent.category.BROWSABLE"/>
                <data android:scheme="qa-admission"/>
            </intent-filter>
        </activity>
    </application>
</manifest>
"@)
    Invoke-Checked (Join-Path $tools 'aapt2.exe') @('link', '-I', $android, '--manifest', $manifest,
        '--min-sdk-version', '28', '--target-sdk-version', '32', '-o', $unsigned)
    Invoke-Checked 'jar' @('-uf', $unsigned, '-C', $dex, 'classes.dex')
    Invoke-Checked (Join-Path $tools 'apksigner.bat') @('sign', '--ks', $key, '--ks-pass', 'pass:android', '--out', $apk, $unsigned)
    Invoke-Checked (Join-Path $tools 'apksigner.bat') @('verify', $apk)
    return $apk
}

$required = '<uses-feature android:name="android.hardware.location" android:required="true"/>'
$optional = '<uses-feature android:name="android.hardware.location" android:required="false"/>'
$requiredApk = Build-Apk 'required' "$packagePrefix.required" $required 1
$optionalApk = Build-Apk 'optional' "$packagePrefix.optional" $optional 1
$permissionApk = Build-Apk 'permission' "$packagePrefix.permission" '' 1
$updateApk = Build-Apk 'update' "$packagePrefix.optional" $required 2
$unknownApk = Build-Apk 'unknown' "$packagePrefix.unknown" '<uses-feature android:name="com.gesher.qa.unknown" android:required="true"/>' 1

function AndroidPath([string] $Path) { ($Path -replace '\\', '/') -replace '^[A-Za-z]:', '' }
$bcp = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') |
    ForEach-Object { AndroidPath "$runtime/$_.jar" }) -join ':'

function Run-Launcher([string] $Label, [string[]] $Command, [string] $ExpectedNotice = '') {
    $log = Join-Path $out "$phase-$Label.log"
    $errorFile = Join-Path $out "$phase-$Label.error.txt"
    $arguments = @("-Xbootclasspath:$bcp", "-Ximage:$(AndroidPath "$profile/system")/framework/boot.art",
        '-Xint', '-Xusejit:false', 'com.gesher.app.Launcher', '--launch-error-file', (AndroidPath $errorFile)) + $Command
    $start = [Diagnostics.ProcessStartInfo]::new("$runtime/dalvikvm.exe")
    $start.Arguments = ($arguments | ForEach-Object {
        if ($_ -match '[\s"]') { '"' + ($_ -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"' } else { $_ }
    }) -join ' '
    $start.WorkingDirectory = $runtime
    $start.UseShellExecute = $false
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $process = [Diagnostics.Process]::Start($start)
    try {
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (!$process.WaitForExit(90000)) { $process.Kill(); throw "Timed out in $phase/$Label" }
        if (![Threading.Tasks.Task]::WaitAll([Threading.Tasks.Task[]]@($stdout, $stderr), 5000)) {
            throw "Launcher streams remained open in $phase/$Label"
        }
        [IO.File]::WriteAllText($log, "$($stdout.Result)`n$($stderr.Result)")
        if ($ExpectedNotice) {
            if ($process.ExitCode -ne 1 -or !(Test-Path -LiteralPath $errorFile) -or
                [IO.File]::ReadAllText($errorFile) -cne $ExpectedNotice) {
                throw "$phase/$Label did not return the expected edition notice. See $log"
            }
        } elseif ($process.ExitCode -ne 0) {
            throw "$phase/$Label failed with exit $($process.ExitCode). See $log"
        }
        return $stdout.Result
    } finally {
        $process.Dispose()
    }
}

function Set-Profile([string] $Name, [string] $Locale, [bool] $Trial) {
    $script:profile = Join-Path $out $Name
    & "$PSScriptRoot/AppContainerPoc.ps1" -Dir $runtime -Root $profile -InitializeOnly
    $env:ANDROID_ROOT = AndroidPath "$profile/system"
    $env:ANDROID_ART_ROOT = AndroidPath "$profile/art"
    $env:ANDROID_DATA = AndroidPath "$profile/data"
    $env:ANDROID_I18N_ROOT = AndroidPath "$profile/i18n"
    $env:ANDROID_TZDATA_ROOT = AndroidPath "$profile/tzdata"
    $env:GESHER_LOGS = AndroidPath "$profile/logs"
    $env:GESHER_LOCALES = $Locale
    if ($Trial) { $env:GESHER_EDITION_TRIAL = 'free' }
    else { Remove-Item Env:GESHER_EDITION_TRIAL -ErrorAction SilentlyContinue }
}

$env:GESHER_APP_WINDOW = 'headless'
$env:GESHER_SHORTCUTS = '0'
$env:GESHER_SANDBOX = 'appcontainer'
$env:GESHER_SANDBOX_NETWORK = 'internet-only'
$env:GESHER_HWUI_RENDERER = 'skiacpu'
$env:GESHER_IP_GEOLOCATION = '0'
$catalogPath = Join-Path $PSScriptRoot '../../../tools/package/ui-messages.json'
$messages = (Get-Content -LiteralPath $catalogPath -Raw -Encoding UTF8 | ConvertFrom-Json).messages
function Trial-Notice([string] $Language, [string] $Package) {
    $messages.edition_trial_limit.$Language.Replace('{0}', $Package).Replace('{1}', $messages.edition_feature_location.$Language)
}

$phase = 'fresh-trial'
Set-Profile 'fresh' 'en-US' $true
try {
    $null = Run-Launcher 'install-required' @('install', (AndroidPath $requiredApk)) (Trial-Notice 'en' "$packagePrefix.required")
    if ((Run-Launcher 'list' @('list')).Contains("package:$packagePrefix.required ")) { throw 'Refused package was published' }
    foreach ($apk in $optionalApk, $permissionApk, $unknownApk) {
        $name = [IO.Path]::GetFileNameWithoutExtension($apk)
        $null = Run-Launcher "install-$name" @('install', (AndroidPath $apk))
        $launch = Run-Launcher "launch-$name" @('launch', "$packagePrefix.$name", '--wait')
        if ($launch -notmatch 'EDITION_ADMISSION_ACTIVITY_ENTERED') { throw "The $name activity did not run" }
    }
    $installed = @(Get-ChildItem -LiteralPath "$profile/data/app" -Directory -Filter "$packagePrefix.optional-*")
    if ($installed.Count -ne 1) { throw 'Expected one installed optional APK generation' }
    $oldApk = Join-Path $installed[0].FullName 'base.apk'
    $original = (Get-FileHash -LiteralPath $oldApk).Hash
    $null = Run-Launcher 'update-required' @('install', (AndroidPath $updateApk)) (Trial-Notice 'en' "$packagePrefix.optional")
    $generations = @(Get-ChildItem -LiteralPath "$profile/data/app" -Directory -Filter "$packagePrefix.optional-*")
    if ($generations.Count -ne 1 -or (Get-FileHash -LiteralPath $oldApk).Hash -cne $original) {
        throw 'A rejected update changed the installed code'
    }
    $null = Run-Launcher 'view-unchanged' @('view', "$packagePrefix.optional", 'qa-admission://fixture/open')
} finally { $null = Run-Launcher 'shutdown' @('shutdown') }

$phase = 'legacy'
Set-Profile 'retained' 'en-US' $false
try {
    $null = Run-Launcher 'install-required' @('install', (AndroidPath $requiredApk))
    $launch = Run-Launcher 'launch-required' @('launch', "$packagePrefix.required", '--wait')
    if ($launch -notmatch 'EDITION_ADMISSION_ACTIVITY_ENTERED') { throw 'The legacy control activity did not run' }
    $null = Run-Launcher 'view-required' @('view', "$packagePrefix.required", 'qa-admission://fixture/open')
} finally { $null = Run-Launcher 'shutdown' @('shutdown') }

$phase = 'retained-trial'
Set-Profile 'retained' 'he-IL' $true
try {
    $notice = Trial-Notice 'he' "$packagePrefix.required"
    $null = Run-Launcher 'reinstall-required' @('install', (AndroidPath $requiredApk)) $notice
    $null = Run-Launcher 'launch-required' @('launch', "$packagePrefix.required", '--wait') $notice
    $null = Run-Launcher 'view-required' @('view', "$packagePrefix.required", 'qa-admission://fixture/open') $notice
    if (!(Run-Launcher 'list' @('list')).Contains("package:$packagePrefix.required ")) { throw 'The preexisting package was removed' }
} finally { $null = Run-Launcher 'shutdown' @('shutdown') }

Write-Output 'PASS: signed APK edition admission, retained package, rejected update, and localized Windows launch notices'
