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

Check 'services: app and channel notification settings open Windows controls; unsupported intents and failed launches stay unresolved' {
  & "$Dir/dalvikvm.exe" "-Xbootclasspath:$bcpBrowser" "-Ximage:$(NoDrive "$root/system")/framework/boot.art" `
    -Xint '-Xusejit:false' -cp (NoDrive "$Dir/services-tests.dex") gesher.services.NotificationSettingsIntentTest
} @('NotificationSettingsIntentTest all passed')
