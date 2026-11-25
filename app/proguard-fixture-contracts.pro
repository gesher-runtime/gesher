# ABI used by separately compiled ART/Windows contract fixtures.
#
# These rules apply to the SAME gesher-app.jar that is packaged and installed.
# There is no weaker test-only framework: services-tests.dex, graphics-tests.dex,
# web3-query and the test APKs must exercise the actual production R8 output.
#
# Service fixtures construct class names from "com.gesher.server." plus runtime
# names, enumerate nested types, and inspect private fields/constructors. Keep the
# service namespace conservatively until those fixtures use stable external APIs.
# This deliberately limits service obfuscation; it does not keep all com.gesher.
-keep,includedescriptorclasses class com.gesher.server.** { *; }

# Other runtime consumers use these exact class families. Nested types are part
# of their descriptors and reflection paths. Descriptor classes keep their names
# as well; their unrelated members remain eligible for obfuscation. Inventory covers the DEX suites,
# test APKs, web3-query, native/font and exec/Binder fixtures. Host-only tests that
# compile their own implementation source do not require production keep rules.
#
# Keep this file in the simple one-line form understood by R8ContractCheck.py,
# which verifies the actual output declarations. fixture_api.py additionally
# resolves references from the separately compiled consumer DEX files.
-keep,includedescriptorclasses class com.gesher.app.BuildProps { *; }
-keep,includedescriptorclasses class com.gesher.app.BuildProps$** { *; }
-keep,includedescriptorclasses class com.gesher.app.DeviceProfile { *; }
-keep,includedescriptorclasses class com.gesher.app.DeviceProfile$** { *; }
-keep,includedescriptorclasses class com.gesher.app.Launcher { *; }
-keep,includedescriptorclasses class com.gesher.app.Launcher$** { *; }
-keep,includedescriptorclasses class com.gesher.app.LogLines { *; }
-keep,includedescriptorclasses class com.gesher.app.LogLines$** { *; }
-keep,includedescriptorclasses class com.gesher.app.Main { *; }
-keep,includedescriptorclasses class com.gesher.app.Main$** { *; }
-keep,includedescriptorclasses class com.gesher.app.NativeLibs { *; }
-keep,includedescriptorclasses class com.gesher.app.NativeLibs$** { *; }
-keep,includedescriptorclasses class com.gesher.app.VmCommandLine { *; }
-keep,includedescriptorclasses class com.gesher.app.VmCommandLine$** { *; }
-keep,includedescriptorclasses class com.gesher.app.accessibility.UiaBridge { *; }
-keep,includedescriptorclasses class com.gesher.app.accessibility.UiaBridge$** { *; }
-keep,includedescriptorclasses class com.gesher.app.fonts.SystemFontMap { *; }
-keep,includedescriptorclasses class com.gesher.app.fonts.SystemFontMap$** { *; }
-keep,includedescriptorclasses class com.gesher.app.fonts.WindowsFonts { *; }
-keep,includedescriptorclasses class com.gesher.app.fonts.WindowsFonts$** { *; }
-keep,includedescriptorclasses class com.gesher.app.graphics.GraphicsDebug { *; }
-keep,includedescriptorclasses class com.gesher.app.graphics.GraphicsDebug$** { *; }
-keep,includedescriptorclasses class com.gesher.app.input.CursorAnchorGeometry { *; }
-keep,includedescriptorclasses class com.gesher.app.input.CursorAnchorGeometry$** { *; }
-keep,includedescriptorclasses class com.gesher.app.input.VirtualKeyboard { *; }
-keep,includedescriptorclasses class com.gesher.app.input.VirtualKeyboard$** { *; }
-keep,includedescriptorclasses class com.gesher.app.input.WindowInput { *; }
-keep,includedescriptorclasses class com.gesher.app.input.WindowInput$** { *; }
-keep,includedescriptorclasses class com.gesher.app.input.WindowsInputMethod { *; }
-keep,includedescriptorclasses class com.gesher.app.input.WindowsInputMethod$** { *; }
-keep,includedescriptorclasses class com.gesher.app.locale.SystemLocale { *; }
-keep,includedescriptorclasses class com.gesher.app.locale.SystemLocale$** { *; }
-keep,includedescriptorclasses class com.gesher.app.locale.UiMessages { *; }
-keep,includedescriptorclasses class com.gesher.app.locale.UiMessages$** { *; }
-keep,includedescriptorclasses class com.gesher.app.shell.AppIcon { *; }
-keep,includedescriptorclasses class com.gesher.app.shell.AppIcon$** { *; }
-keep,includedescriptorclasses class com.gesher.app.shell.AppShell { *; }
-keep,includedescriptorclasses class com.gesher.app.shell.AppShell$** { *; }
-keep,includedescriptorclasses class com.gesher.app.shell.IconApks { *; }
-keep,includedescriptorclasses class com.gesher.app.shell.IconApks$** { *; }
-keep,includedescriptorclasses class com.gesher.app.shell.Shell { *; }
-keep,includedescriptorclasses class com.gesher.app.shell.Shell$** { *; }
-keep,includedescriptorclasses class com.gesher.app.webview.GesherCookieManager { *; }
-keep,includedescriptorclasses class com.gesher.app.webview.GesherCookieManager$** { *; }
-keep,includedescriptorclasses class com.gesher.app.webview.GesherWebMessagePort { *; }
-keep,includedescriptorclasses class com.gesher.app.webview.GesherWebMessagePort$** { *; }
-keep,includedescriptorclasses class com.gesher.app.webview.GesherWebStorage { *; }
-keep,includedescriptorclasses class com.gesher.app.webview.GesherWebStorage$** { *; }
-keep,includedescriptorclasses class com.gesher.app.webview.GesherWebView { *; }
-keep,includedescriptorclasses class com.gesher.app.webview.GesherWebView$** { *; }
-keep,includedescriptorclasses class com.gesher.app.webview.WebView2Host { *; }
-keep,includedescriptorclasses class com.gesher.app.webview.WebView2Host$** { *; }
-keep,includedescriptorclasses class com.gesher.app.window.ActivityClientHook { *; }
-keep,includedescriptorclasses class com.gesher.app.window.ActivityClientHook$** { *; }
-keep,includedescriptorclasses class com.gesher.app.window.AppWindowTasks { *; }
-keep,includedescriptorclasses class com.gesher.app.window.AppWindowTasks$** { *; }
-keep,includedescriptorclasses class com.gesher.app.window.AppWindows { *; }
-keep,includedescriptorclasses class com.gesher.app.window.AppWindows$** { *; }
-keep,includedescriptorclasses class com.gesher.app.window.ApplicationDisplay { *; }
-keep,includedescriptorclasses class com.gesher.app.window.ApplicationDisplay$** { *; }
-keep,includedescriptorclasses class com.gesher.app.window.NativeWindow { *; }
-keep,includedescriptorclasses class com.gesher.app.window.NativeWindow$** { *; }
-keep,includedescriptorclasses class com.gesher.app.window.TaskConfiguration { *; }
-keep,includedescriptorclasses class com.gesher.app.window.TaskConfiguration$** { *; }
-keep,includedescriptorclasses class com.gesher.app.window.WindowSession { *; }
-keep,includedescriptorclasses class com.gesher.app.window.WindowSession$** { *; }
-keep,includedescriptorclasses class com.gesher.net.IConnectivity { *; }
-keep,includedescriptorclasses class com.gesher.net.IConnectivity$** { *; }
-keep,includedescriptorclasses class com.gesher.net.IWindowsWifi { *; }
-keep,includedescriptorclasses class com.gesher.net.IWindowsWifi$** { *; }
-keep,includedescriptorclasses class com.gesher.net.ProxySetting { *; }
-keep,includedescriptorclasses class com.gesher.net.ProxySetting$** { *; }
-keep,includedescriptorclasses class com.gesher.net.WindowsNetworks { *; }
-keep,includedescriptorclasses class com.gesher.net.WindowsNetworks$** { *; }
-keep,includedescriptorclasses class com.gesher.net.WindowsNsd { *; }
-keep,includedescriptorclasses class com.gesher.net.WindowsNsd$** { *; }
-keep,includedescriptorclasses class com.gesher.net.WindowsProxy { *; }
-keep,includedescriptorclasses class com.gesher.net.WindowsProxy$** { *; }
-keep,includedescriptorclasses class com.gesher.net.WindowsWifi { *; }
-keep,includedescriptorclasses class com.gesher.net.WindowsWifi$** { *; }
-keep,includedescriptorclasses class com.gesher.net.WlanProfiles { *; }
-keep,includedescriptorclasses class com.gesher.net.WlanProfiles$** { *; }
