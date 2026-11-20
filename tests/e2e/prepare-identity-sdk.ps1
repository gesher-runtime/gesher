# Install the same SDK inspection tools for the real collector and its fixtures.
# Hosted Linux/Windows runners provide the Android SDK and accepted licenses.
$ErrorActionPreference = 'Stop'
$sdk = $env:ANDROID_HOME
if (-not $sdk) { $sdk = $env:ANDROID_SDK_ROOT }
if (-not $sdk) { throw 'ANDROID_HOME or ANDROID_SDK_ROOT is required for APK identity tests' }
$suffix = if ($IsWindows) { '.bat' } else { '' }
$manager = Join-Path $sdk "cmdline-tools/latest/bin/sdkmanager$suffix"
if (-not (Test-Path -LiteralPath $manager -PathType Leaf)) { throw 'Android SDK manager is unavailable' }
& $manager 'build-tools;36.1.0' 'platforms;android-36'
if ($LASTEXITCODE -ne 0) { throw "Android SDK installation failed: $LASTEXITCODE" }
$env:GESHER_TEST_BUILD_TOOLS = Join-Path $sdk 'build-tools/36.1.0'
$env:GESHER_TEST_ANDROID_JAR = Join-Path $sdk 'platforms/android-36/android.jar'
$env:GESHER_REQUIRE_REAL_APK_IDENTITY_TEST = '1'
if ($env:GITHUB_ENV) {
  "GESHER_TEST_BUILD_TOOLS=$env:GESHER_TEST_BUILD_TOOLS" >> $env:GITHUB_ENV
  "GESHER_TEST_ANDROID_JAR=$env:GESHER_TEST_ANDROID_JAR" >> $env:GITHUB_ENV
  'GESHER_REQUIRE_REAL_APK_IDENTITY_TEST=1' >> $env:GITHUB_ENV
}
