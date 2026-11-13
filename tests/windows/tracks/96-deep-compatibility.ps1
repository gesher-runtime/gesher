# Additional compatibility contracts beyond PRs #10, #11 and #12.
# FixesRun and the actual ART boot arguments are provided by 94-framework-fixes.
$deepWifiOut = FixesRun ($dvmFixes + 'gesher.services.WifiDataContractTest') 90
$deepWifiCode = $LASTEXITCODE
Check 'framework: Wi-Fi data APIs and real native Parcelable round trips' {
  $global:LASTEXITCODE = $deepWifiCode
  $deepWifiOut
} @('WifiDataContractTest all passed:', 'parcel=true')
