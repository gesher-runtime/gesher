# R8 configuration for gesher-app.jar (bootclasspath overlay & system server).
# Keeps framework-facing overlay packages, JNI/native-called classes,
# Binder stubs, and CLI entry points while shrinking and obfuscating
# internal com.gesher.* implementation classes, methods, and fields.

-optimizationpasses 3
-allowaccessmodification
-overloadaggressively
-repackageclasses 'g'
-dontwarn **

# Bootclasspath APIs are libraries consumed by other DEX files. Their reflective
# type/annotation metadata is part of that API (for example enum declaring
# classes and Parcelable.Creator<T> signatures), even when all members are kept.
-keepattributes Exceptions,Signature,InnerClasses,EnclosingMethod,*Annotation*

# Keep bootclasspath overlay packages referenced by framework.jar and apps
# (verified by app/gen/BootLinkCheck.java).
-keep,includedescriptorclasses class android.** { *; }
-keep class android.app.GesherAppDriver { *; }
-keep class android.app.GesherAppProcess { *; }
-keep class android.net.http.EngineHttpCache { *; }
-keep,includedescriptorclasses class org.ccil.cowan.tagsoup.** { *; }
-keep,includedescriptorclasses class com.android.internal.location.** { *; }
-keep public class com.android.apksig.** { *; }
-keep,includedescriptorclasses class com.android.net.module.** { *; }
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
-keep class com.gesher.server.WindowsSpeech$** { *; }
-keep class com.gesher.server.WindowsSpeechRecognition { *; }
-keep class com.gesher.server.WindowsSpeechRecognition$** { *; }

# AndroidX calls these delegates through InvocationHandler and looks up the
# application's boundary Method by name, not through a Java interface implemented
# by the delegate. Keeping only SupportLibReflectionUtil leaves these methods
# invisible to R8 and removes the advertised WebView features.
-keep public class com.gesher.app.webview.SupportLib$* {
    public <methods>;
}

# JCA loads providers and their SPI implementations by class names stored in
# security properties / Provider entries. Class.getName() alone does not retain
# the constructors invoked by Provider.Service.newInstance(). The private key
# storage and digest implementation helpers remain eligible for optimization.
-keep class com.gesher.app.AndroidKeyStore { public <init>(); }
-keep class com.gesher.app.Digests { public <init>(); }
-keep public class com.gesher.app.AndroidKeyStore$* { public <init>(); }
-keep public class com.gesher.app.Digests$* { public <init>(); }

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
