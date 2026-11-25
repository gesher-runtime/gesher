# R8 configuration for gesher-app.jar (bootclasspath overlay & system server).
# Keeps framework-facing overlay packages, JNI/native-called classes,
# Binder stubs, and CLI entry points while shrinking and obfuscating
# internal com.gesher.* implementation classes, methods, and fields.

-optimizationpasses 3
-allowaccessmodification
-overloadaggressively
-repackageclasses 'g'
-dontwarn **

# Keep bootclasspath overlay packages referenced by framework.jar and apps
# (verified by app/gen/BootLinkCheck.java).
-keep public class android.** { *; }
-keep class android.app.GesherAppDriver { *; }
-keep class android.app.GesherAppProcess { *; }
-keep class android.net.http.EngineHttpCache { *; }
-keep public class org.ccil.cowan.tagsoup.** { *; }
-keep public class com.android.internal.location.** { *; }
-keep public class com.android.apksig.** { *; }
-keep public class com.android.net.module.** { *; }
-keep class org.chromium.support_lib_glue.** { *; }

# Keep CLI and process entry points invoked by native launchers / scripts.
-keep class com.gesher.app.Main {
    public static void main(java.lang.String[]);
}
-keep class com.gesher.app.Launcher {
    public static void main(java.lang.String[]);
}
-keep class com.gesher.app.shell.AppShell {
    public static void main(java.lang.String[]);
}
-keep class com.gesher.server.SystemServer {
    public static void main(java.lang.String[]);
}
-keep class com.gesher.server.CameraSelection {
    public static void main(java.lang.String[]);
}
-keep class com.gesher.server.RuntimeCommands {
    public static void main(java.lang.String[]);
}
-keep class com.gesher.server.MicrogAddons {
    public static void main(java.lang.String[]);
}

# Keep cross-APK APIs used by gesher-tts.apk and gesher-stt.apk.
-keep class com.gesher.server.WindowsSpeech { *; }
-keep class com.gesher.server.WindowsSpeechRecognition { *; }

# Keep dynamically instantiated Binder stubs (com.gesher.server.StandIn).
-keep class com.gesher.server.stub.** { *; }

# Keep all JNI classes and their native methods (RegisterNatives & Java_*).
-keepclasseswithmembers,includedescriptorclasses class com.gesher.** {
    native <methods>;
}

# Keep classes and members looked up by name via JNI FindClass / GetMethodID.
-keep class com.gesher.app.accessibility.UiaBridge { *; }
-keep class com.gesher.app.accessibility.UiaClient { *; }
-keep class com.gesher.app.input.WindowInput { *; }
-keep class com.gesher.app.webview.WebView2Host { *; }
-keep class com.gesher.app.window.WindowSession { *; }
-keep class com.gesher.net.WindowsNetworks { *; }
-keep class com.gesher.net.WindowsNsd { *; }
-keep class com.gesher.net.WindowsProxy { *; }
-keep class com.gesher.net.WindowsWifi { *; }
-keep class com.gesher.server.AudioService { *; }
-keep class com.gesher.server.CameraFrames { *; }
-keep class com.gesher.server.LegacyCamera { *; }
-keep class com.gesher.server.WindowsCellular { *; }
-keep class com.gesher.server.Zygote { *; }
