# Install the same SDK inspection tools for the real collector and its fixtures.
# Hosted Linux/Windows runners provide the Android SDK and accepted licenses.
$ErrorActionPreference = 'Stop'
$sdk = $env:ANDROID_HOME
if (-not $sdk) { $sdk = $env:ANDROID_SDK_ROOT }
if (-not $sdk) { throw 'ANDROID_HOME or ANDROID_SDK_ROOT is required for APK identity tests' }
$buildTools = Join-Path $sdk 'build-tools/36.1.0'
$androidJar = Join-Path $sdk 'platforms/android-36/android.jar'
$aapt2 = if ($IsWindows) { 'aapt2.exe' } else { 'aapt2' }
$required = @(
  (Join-Path $buildTools $aapt2)
  (Join-Path $buildTools 'lib/apksigner.jar')
  $androidJar
)

# The hosted images may already provide the exact tools and platform. Only
# contact sdkmanager when a required component is missing.
if ($required | Where-Object { -not (Test-Path -LiteralPath $_ -PathType Leaf) }) {
  $suffix = if ($IsWindows) { '.bat' } else { '' }
  $manager = Join-Path $sdk "cmdline-tools/latest/bin/sdkmanager$suffix"
  if (-not (Test-Path -LiteralPath $manager -PathType Leaf)) { throw 'Android SDK manager is unavailable' }
  & $manager 'build-tools;36.1.0' 'platforms;android-36'
  if ($LASTEXITCODE -ne 0) { throw "Android SDK installation failed: $LASTEXITCODE" }
  $missing = @($required | Where-Object { -not (Test-Path -LiteralPath $_ -PathType Leaf) })
  if ($missing.Count) { throw "Android SDK installation left required files missing: $($missing -join ', ')" }
}

$env:GESHER_TEST_BUILD_TOOLS = $buildTools
$env:GESHER_TEST_ANDROID_JAR = $androidJar
$env:GESHER_REQUIRE_REAL_APK_IDENTITY_TEST = '1'
if ($env:GITHUB_ENV) {
  "GESHER_TEST_BUILD_TOOLS=$env:GESHER_TEST_BUILD_TOOLS" >> $env:GITHUB_ENV
  "GESHER_TEST_ANDROID_JAR=$env:GESHER_TEST_ANDROID_JAR" >> $env:GITHUB_ENV
  'GESHER_REQUIRE_REAL_APK_IDENTITY_TEST=1' >> $env:GITHUB_ENV
}
