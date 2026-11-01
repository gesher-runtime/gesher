# Unit tests for GMS and microG compatibility layer
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$work = Join-Path $env:TEMP "gms-unit-test-$([System.Guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $work -Force | Out-Null

try {
  $sources = @(
    (Get-ChildItem -Path "$root/tests/test-apps/stubs" -Recurse -Filter "*.java" | Select-Object -ExpandProperty FullName),
    (Get-ChildItem -Path "$root/app/java/com/google/android/gms/tasks" -Recurse -Filter "*.java" | Select-Object -ExpandProperty FullName),
    (Get-ChildItem -Path "$root/app/java/com/google/android/gms/common" -Recurse -Filter "*.java" | Select-Object -ExpandProperty FullName),
    (Get-ChildItem -Path "$root/app/java/com/google/android/gms/auth" -Recurse -Filter "*.java" | Select-Object -ExpandProperty FullName),
    "$root/app/java/com/google/android/gms/location/Priority.java",
    "$root/app/java/com/google/android/gms/location/LocationRequest.java",
    "$root/app/java/com/google/android/gms/location/LocationResult.java",
    "$root/app/java/com/google/android/gms/location/LocationAvailability.java",
    "$root/app/java/com/google/android/gms/location/LocationCallback.java",
    "$root/app/java/com/google/android/gms/location/FusedLocationProviderClient.java",
    "$root/app/java/com/google/android/gms/location/LocationServices.java",
    "$root/app/java/com/google/android/gms/location/SettingsClient.java",
    "$root/app/java/com/google/android/gms/location/LocationSettingsRequest.java",
    "$root/app/java/com/google/android/gms/location/LocationSettingsResponse.java",
    "$root/app/java/com/google/android/gms/location/LocationSettingsStates.java",
    "$root/app/java/com/google/firebase/FirebaseApp.java",
    "$root/app/java/com/google/firebase/FirebaseOptions.java",
    "$root/app/java/com/google/firebase/messaging/RemoteMessage.java",
    "$root/app/java/com/google/firebase/messaging/FirebaseMessaging.java",
    "$root/app/java/com/gesher/server/GmsPackageSupport.java",
    "$root/tests/test-apps/GmsCompatibilityUnitTest.java"
  )

  $sourcesFile = Join-Path $work "sources.txt"
  $sources | Set-Content $sourcesFile

  Write-Host "Compiling GMS unit tests..."
  & javac --release 11 -d $work "@$sourcesFile"
  if ($LASTEXITCODE -ne 0) { throw "javac failed with code $LASTEXITCODE" }

  Write-Host "Executing GmsCompatibilityUnitTest..."
  & java -cp $work com.gesher.tests.GmsCompatibilityUnitTest
  if ($LASTEXITCODE -ne 0) { throw "GmsCompatibilityUnitTest failed with code $LASTEXITCODE" }

  Write-Host "PASS  GmsCompatibilityUnitTest"
} finally {
  Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
