# The Windows default browser as an Android package (com.gesher.browser,
# BrowserPackageSupport): apps that look for a browser before they open a page
# (AppAuth's BrowserSelector, as Thunderbird's OAuth sign-in does) find one, and
# starting it delegates the URL to Windows. Run from services-tests.dex
# (tests/dex-services) with the interpreter; no browser opens (the test replaces
# ActivityTaskManagerService's URL opener). The URL protocols that bring an OAuth
# redirect back are checked against the installed Gesher, in the package job
# (.github/workflows/ci.yml). Dot-sourced by run-tests.ps1, using Check, NoDrive,
# $Dir, $root and $failed from there.

Copy-Item "$Dir/framework-res.apk" (Join-Path $root 'system/framework') -Force
Copy-Item "$Dir/build.prop" (Join-Path $root 'system') -Force
$bcpBrowser = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
Check 'services: a browser for apps that look for one (com.gesher.browser: VIEW http(s) BROWSABLE, its signature, no custom tabs); starting it opens the URL in Windows' {
  & "$Dir/dalvikvm.exe" "-Xbootclasspath:$bcpBrowser" "-Ximage:$(NoDrive "$root/system")/framework/boot.art" `
    -Xint '-Xusejit:false' -cp (NoDrive "$Dir/services-tests.dex") gesher.services.BrowserIntentTest
} @('BrowserIntentTest: queryIntentActivities(VIEW http BROWSABLE) finds com.gesher.browser',
  'BrowserIntentTest: getPackageInfo(com.gesher.browser, GET_SIGNATURES) has its signature; no custom tabs service',
  'BrowserIntentTest: starting VIEW http(s) for com.gesher.browser (package, component, implicit) delegates the URL to Windows',
  'BrowserIntentTest all passed')

# Windows' mailto/tel/sms/geo protocol handlers as Android activities (WindowsUriHandlers):
# each resolves and opens exactly when this machine's Windows has a handler registered for
# the scheme (AssocQueryStringW), and is ActivityNotFoundException otherwise. The opener is
# replaced: nothing opens. The per-scheme lines show the runner's own associations.
Check 'services: mailto/tel/sms/geo resolve and open only through a registered Windows handler; otherwise no activity' {
  & "$Dir/dalvikvm.exe" "-Xbootclasspath:$bcpBrowser" "-Ximage:$(NoDrive "$root/system")/framework/boot.art" `
    -Xint '-Xusejit:false' -cp (NoDrive "$Dir/services-tests.dex") gesher.services.UriHandlerIntentTest
} @('UriHandlerIntentTest: mailto Windows handler',
  'UriHandlerIntentTest: tel Windows handler',
  'UriHandlerIntentTest: sms Windows handler',
  'UriHandlerIntentTest: geo Windows handler',
  'UriHandlerIntentTest: CALL, mms and SEND message/rfc822 have no Windows handler activity',
  'UriHandlerIntentTest all passed')

Check 'services: app and channel notification settings open Windows controls; unsupported intents and failed launches stay unresolved' {
  & "$Dir/dalvikvm.exe" "-Xbootclasspath:$bcpBrowser" "-Ximage:$(NoDrive "$root/system")/framework/boot.art" `
    -Xint '-Xusejit:false' -cp (NoDrive "$Dir/services-tests.dex") gesher.services.NotificationSettingsIntentTest
} @('NotificationSettingsIntentTest all passed')

Check 'services: chooser and intent sharing contracts (ACTION_CHOOSER)' {
  & "$Dir/dalvikvm.exe" "-Xbootclasspath:$bcpBrowser" "-Ximage:$(NoDrive "$root/system")/framework/boot.art" `
    -Xint '-Xusejit:false' -cp (NoDrive "$Dir/services-tests.dex") gesher.services.ChooserContractTest
} @('ChooserContractTest all passed:')

# Android Settings screens (ADR 0087): an app's "App info" opens the Control Center on it,
# device screens a fixed Windows Settings page (both openers replaced: nothing opens), and
# the stand-in answers that were wrong (component states, storage stats, memory info).
Check 'services: Settings screens reach the Control Center or Windows Settings; component states, storage stats and memory info answer as Android' {
  & "$Dir/dalvikvm.exe" "-Xbootclasspath:$bcpBrowser" "-Ximage:$(NoDrive "$root/system")/framework/boot.art" `
    -Xint '-Xusejit:false' -cp (NoDrive "$Dir/services-tests.dex") gesher.services.SettingsIntentsTest (NoDrive "$Dir/components.apk")
} @('SettingsIntentsTest: Settings screens reach the Control Center or Windows Settings',
  'SettingsIntentsTest: component enabled states are kept and applied',
  'SettingsIntentsTest: storage stats measure the app''s files',
  'SettingsIntentsTest all passed:')

Check 'services: concurrent Control Center requests stay whole on the runtime filesystem' {
  & "$Dir/dalvikvm.exe" "-Xbootclasspath:$bcpBrowser" "-Ximage:$(NoDrive "$root/system")/framework/boot.art" `
    -Xint '-Xusejit:false' -cp (NoDrive "$Dir/services-tests.dex") gesher.services.ControlCenterRequestsTest
} @('ControlCenterRequestsTest all passed:')

# country_detector (CountryDetectorService over WindowsCountry): this machine's Windows
# "Country or region", or the default Locale's country without one, as
# COUNTRY_SOURCE_LOCALE, through StandIn's generated ICountryDetector stub.
Check 'services: CountryDetector reports Windows'' Country or region as the locale source' {
  & "$Dir/dalvikvm.exe" "-Xbootclasspath:$bcpBrowser" "-Ximage:$(NoDrive "$root/system")/framework/boot.art" `
    -Xint '-Xusejit:false' -cp (NoDrive "$Dir/services-tests.dex") gesher.services.CountryDetectorTest
} @('CountryDetectorTest: detectCountry is ',
  'CountryDetectorTest all passed')
