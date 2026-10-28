# CI-only full regression, preserving the qualified ordinary driver and all cases.
$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -cne 'true') { throw 'CI only' }
$root = Join-Path $env:RUNNER_TEMP ('gesher-binder-full-' + [guid]::NewGuid().ToString('N'))
foreach ($folder in 'system/framework/x86_64','system/fonts','system/etc','art','data','i18n/etc/icu','tzdata') {
  New-Item -ItemType Directory -Force (Join-Path $root $folder) | Out-Null
}
Copy-Item dist/icudt*l.dat (Join-Path $root 'i18n/etc/icu')
Copy-Item dist/framework-res.apk (Join-Path $root 'system/framework')
Copy-Item dist/build.prop (Join-Path $root 'system')
Copy-Item dist/fonts/*.ttf (Join-Path $root 'system/fonts')
Copy-Item dist/fonts/fonts.xml (Join-Path $root 'system/etc')
Copy-Item -Recurse -Force dist/tzdata/* (Join-Path $root 'tzdata')
Copy-Item boot-image/framework/x86_64/* (Join-Path $root 'system/framework/x86_64')
foreach ($jar in 'core-oj','core-libart','core-icu4j','framework','gesher-app') {
  Write-Host "ACTUAL_SG_JAR member=$jar.jar sha256=$((Get-FileHash "dist/$jar.jar" -Algorithm SHA256).Hash) source=$env:GITHUB_SHA"
}
foreach ($image in 'boot','boot-core-libart','boot-core-icu4j','boot-framework','boot-gesher-app') {
  foreach ($extension in 'art','oat','vdex') {
    $member = "$image.$extension"
    $produced = (Get-FileHash "boot-image/framework/x86_64/$member" -Algorithm SHA256).Hash
    $consumed = (Get-FileHash "$root/system/framework/x86_64/$member" -Algorithm SHA256).Hash
    if ($produced -cne $consumed) { throw "SG image copy identity mismatch $member" }
    Write-Host "ACTUAL_SG_IMAGE member=$member sha256=$consumed current-source-rebuilt-copy-identical=True"
  }
}
if ((Get-Content dist/binder-sg-runtime-source.txt -Raw).Trim() -cne $env:GITHUB_SHA) {
  throw 'SG selected runtime source marker mismatch'
}
Write-Host "ACTUAL_SG_TRANSLATOR sha256=$((Get-FileHash dist/gesher_arm64.dll -Algorithm SHA256).Hash) source=$env:GITHUB_SHA"
$log = Join-Path $PWD 'binder-full-output.txt'
& pwsh -NoProfile -File tests/binder/run-modes.ps1 -Dir "$PWD/dist" -Fixtures "$PWD/fixtures" -RuntimeRoot $root -Source $env:GITHUB_SHA 2>&1 | Tee-Object -FilePath $log
$code = $LASTEXITCODE
if ($code -ne 0) { exit $code }
python tools/binder/verify-full-output.py $log
exit $LASTEXITCODE
