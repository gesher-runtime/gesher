// This allowlist is expanded for both [Files] and the installed SHA-256 checks.
// Each entry names one production source file and its directory beneath runtime.

// Device processes, guest subprocesses and packaged runtime helpers.
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "dalvikvm.exe", "")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "binderd.exe", "")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "servicemanager.exe", "")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "gesher-exec.exe", "")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "dex2oat.exe", "")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "profman.exe", "")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "libgesher_webview_bridge.so", "")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "libgesher_account_crypto.so", "")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "libEGL.dll", "")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "libGLESv2.dll", "")

// ART boot class path. Preserve this order in the launcher.
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "core-oj.jar", "")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "core-libart.jar", "")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "core-icu4j.jar", "")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "framework.jar", "")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "gesher-app.jar", "")

// The complete matching x64 ART image from the boot-image supplier.
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "framework\x86_64\boot.art", "\framework\x86_64")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "framework\x86_64\boot.oat", "\framework\x86_64")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "framework\x86_64\boot.vdex", "\framework\x86_64")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "framework\x86_64\boot-core-libart.art", "\framework\x86_64")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "framework\x86_64\boot-core-libart.oat", "\framework\x86_64")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "framework\x86_64\boot-core-libart.vdex", "\framework\x86_64")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "framework\x86_64\boot-core-icu4j.art", "\framework\x86_64")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "framework\x86_64\boot-core-icu4j.oat", "\framework\x86_64")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "framework\x86_64\boot-core-icu4j.vdex", "\framework\x86_64")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "framework\x86_64\boot-framework.art", "\framework\x86_64")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "framework\x86_64\boot-framework.oat", "\framework\x86_64")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "framework\x86_64\boot-framework.vdex", "\framework\x86_64")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "framework\x86_64\boot-gesher-app.art", "\framework\x86_64")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "framework\x86_64\boot-gesher-app.oat", "\framework\x86_64")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "framework\x86_64\boot-gesher-app.vdex", "\framework\x86_64")

// Immutable Android system and ICU files.
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "build.prop", "\system")

// Framework resources and the standard shared-library implementations.
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "framework-res.apk", "\system\framework")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "org.apache.http.legacy.jar", "\system\framework")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "com.android.location.provider.jar", "\system\framework")

// Shared-library declarations from this source checkout.
#emit EmitRuntimeFile(AddBackslash(SourcePath) + "etc\permissions\org.apache.http.legacy.xml", "\system\etc\permissions")
#emit EmitRuntimeFile(AddBackslash(SourcePath) + "etc\permissions\com.android.location.provider.xml", "\system\etc\permissions")

// ICU data in its Android path.
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "icudt76l.dat", "\i18n\etc\icu")

// Font configuration and packages consumed by the framework.
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "fonts\fonts.xml", "\system\etc")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "fonts\windows-fonts.xml", "\system\etc")

// Fonts from the production installer.
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "fonts\DroidSansMono.ttf", "\system\fonts")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "fonts\NotoNaskhArabic-Bold.ttf", "\system\fonts")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "fonts\NotoNaskhArabic-Regular.ttf", "\system\fonts")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "fonts\NotoNaskhArabicUI-Bold.ttf", "\system\fonts")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "fonts\NotoNaskhArabicUI-Regular.ttf", "\system\fonts")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "fonts\NotoSansDevanagari-VF.ttf", "\system\fonts")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "fonts\NotoSansDevanagariUI-VF.ttf", "\system\fonts")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "fonts\NotoSansHebrew-Bold.ttf", "\system\fonts")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "fonts\NotoSansHebrew-Regular.ttf", "\system\fonts")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "fonts\NotoSansThai-Bold.ttf", "\system\fonts")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "fonts\NotoSansThai-Regular.ttf", "\system\fonts")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "fonts\NotoSansThaiUI-Bold.ttf", "\system\fonts")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "fonts\NotoSansThaiUI-Regular.ttf", "\system\fonts")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "fonts\NotoSerif-Bold.ttf", "\system\fonts")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "fonts\NotoSerif-BoldItalic.ttf", "\system\fonts")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "fonts\NotoSerif-Italic.ttf", "\system\fonts")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "fonts\NotoSerif-Regular.ttf", "\system\fonts")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "fonts\Roboto-Regular.ttf", "\system\fonts")

// Production time zone module.
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "tzdata\etc\tz\tzdata", "\tzdata\etc\tz")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "tzdata\etc\tz\tz_version", "\tzdata\etc\tz")

// Versioned time zone module files.
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "tzdata\etc\tz\versioned\9\telephonylookup.xml", "\tzdata\etc\tz\versioned\9")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "tzdata\etc\tz\versioned\9\tzdata", "\tzdata\etc\tz\versioned\9")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "tzdata\etc\tz\versioned\9\tzlookup.xml", "\tzdata\etc\tz\versioned\9")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "tzdata\etc\tz\versioned\9\tz_version", "\tzdata\etc\tz\versioned\9")

// Versioned ICU time zone data.
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "tzdata\etc\tz\versioned\9\icu\metaZones.res", "\tzdata\etc\tz\versioned\9\icu")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "tzdata\etc\tz\versioned\9\icu\timezoneTypes.res", "\tzdata\etc\tz\versioned\9\icu")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "tzdata\etc\tz\versioned\9\icu\windowsZones.res", "\tzdata\etc\tz\versioned\9\icu")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "tzdata\etc\tz\versioned\9\icu\zoneinfo64.res", "\tzdata\etc\tz\versioned\9\icu")

// Notices from the verified suppliers.
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "ANGLE-NOTICES.txt", "\licenses")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "FONTS-NOTICES.txt", "\licenses")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "TZDATA-NOTICES.txt", "\licenses")
#emit EmitRuntimeFile(AddBackslash(BuildDir) + "NET-NOTICES.txt", "\licenses")

// Notices from this source checkout.
#emit EmitRuntimeFile(AddBackslash(SourcePath) + "licenses\APKSIG-LICENSE.txt", "\licenses")
#emit EmitRuntimeFile(AddBackslash(SourcePath) + "licenses\BIONIC-GDTOA-NOTICES.txt", "\licenses")
#emit EmitRuntimeFile(AddBackslash(SourcePath) + "licenses\ICU-CALENDAR-LICENSE.txt", "\licenses")
