/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.content.pm.ApplicationInfo;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.gesher.server.ApkPackage;
import com.gesher.server.SharedLibraryGraph;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import sun.misc.Unsafe;

/**
 * Runs the production metadata projection with real framework types on ART or a host JVM.
 * Only native-dependent constructors are bypassed; the separate split fixture exercises
 * actual manifest parsing, PackageInfo generation, installed paths and DEX/resources.
 */
public final class PackageManifestFlagsTest {
    private static int checks;
    private static final String PACKAGE = "fixture.manifest.flags";

    private static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }

    private static void flag(String name, int flags, int flag, boolean expected) {
        require(((flags & flag) != 0) == expected, "Manifest flag lost or invented: " + name);
    }

    private static void field(Object target, String name, Object value) throws Exception {
        Field field = target.getClass().getDeclaredField(name);
        field.setAccessible(true);
        field.set(target, value);
    }

    private static ApplicationInfo apply(Unsafe unsafe, PackageImpl parsed,
            boolean hasDex, boolean addon) throws Exception {
        ApkPackage apk = (ApkPackage) unsafe.allocateInstance(ApkPackage.class);
        field(apk, "parsed", parsed);
        field(apk, "packageName", PACKAGE);
        field(apk, "apkPath", "/fixture/base.apk");
        field(apk, "mUid", 12345);
        field(apk, "mDataDir", "/fixture/data");
        field(apk, "mHasCode", hasDex);
        field(apk, "mSystemAddon", addon);
        field(apk, "sharedLibraries", SharedLibraryGraph.EMPTY);
        // ApplicationInfo's constructor reads native SystemClock on Android. Its
        // fields are what the production projection accepts; no Android API is mocked.
        ApplicationInfo info = (ApplicationInfo) unsafe.allocateInstance(ApplicationInfo.class);
        info.packageName = PACKAGE;
        info.flags = ApplicationInfo.FLAG_STOPPED;
        info.privateFlags = ApplicationInfo.PRIVATE_FLAG_INSTANT;
        Method installed = ApkPackage.class.getDeclaredMethod("installed", ApplicationInfo.class);
        installed.setAccessible(true);
        installed.invoke(apk, info);
        require(info.uid == 12345 && "/fixture/base.apk".equals(info.sourceDir)
                && "/fixture/data".equals(info.dataDir)
                && "/fixture/data/lib".equals(info.nativeLibraryDir),
                "Manifest projection replaced installed identity or paths");
        flag("installed", info.flags, ApplicationInfo.FLAG_INSTALLED, true);
        flag("existing stopped state", info.flags, ApplicationInfo.FLAG_STOPPED, true);
        flag("existing instant state", info.privateFlags, ApplicationInfo.PRIVATE_FLAG_INSTANT, true);
        flag("actual DEX", info.flags, ApplicationInfo.FLAG_HAS_CODE, hasDex);
        flag("verified addon", info.flags, ApplicationInfo.FLAG_SYSTEM, addon);
        flag("verified addon privilege", info.privateFlags, ApplicationInfo.PRIVATE_FLAG_PRIVILEGED, addon);
        return info;
    }

    private static PackageImpl parsed(boolean enabled) {
        PackageImpl parsed = (PackageImpl) PackageImpl.forTesting(PACKAGE, "/fixture/base.apk");
        parsed.setTargetSdkVersion(36);
        parsed.setDeclaredHavingCode(true);
        parsed.setDebuggable(enabled);
        parsed.setLargeHeap(enabled);
        parsed.setCleartextTrafficAllowed(enabled);
        parsed.setHardwareAccelerated(enabled);
        parsed.setExtractNativeLibrariesRequested(enabled);
        parsed.setRtlSupported(enabled);
        parsed.setBackupAllowed(enabled);
        parsed.setTestOnly(enabled);
        parsed.setIsolatedSplitLoading(enabled);
        parsed.setUseEmbeddedDex(enabled);
        parsed.setDirectBootAware(enabled);
        parsed.setDefaultToDeviceProtectedStorage(enabled);
        parsed.setAllowAudioPlaybackCapture(enabled);
        parsed.setSaveStateDisallowed(enabled);
        parsed.setResizeableActivity(enabled);
        parsed.setOnBackInvokedCallbackEnabled(enabled);
        return parsed;
    }

    private static void manifest(ApplicationInfo info, boolean expected) {
        flag("debuggable", info.flags, ApplicationInfo.FLAG_DEBUGGABLE, expected);
        flag("large heap", info.flags, ApplicationInfo.FLAG_LARGE_HEAP, expected);
        flag("cleartext traffic", info.flags, ApplicationInfo.FLAG_USES_CLEARTEXT_TRAFFIC, expected);
        flag("hardware acceleration", info.flags, ApplicationInfo.FLAG_HARDWARE_ACCELERATED, expected);
        flag("native extraction", info.flags, ApplicationInfo.FLAG_EXTRACT_NATIVE_LIBS, expected);
        flag("RTL", info.flags, ApplicationInfo.FLAG_SUPPORTS_RTL, expected);
        flag("backup", info.flags, ApplicationInfo.FLAG_ALLOW_BACKUP, expected);
        flag("test only", info.flags, ApplicationInfo.FLAG_TEST_ONLY, expected);
        flag("isolated splits", info.privateFlags,
                ApplicationInfo.PRIVATE_FLAG_ISOLATED_SPLIT_LOADING, expected);
        flag("embedded DEX", info.privateFlags, ApplicationInfo.PRIVATE_FLAG_USE_EMBEDDED_DEX, expected);
        flag("direct boot", info.privateFlags, ApplicationInfo.PRIVATE_FLAG_DIRECT_BOOT_AWARE, expected);
        flag("device protected default", info.privateFlags,
                ApplicationInfo.PRIVATE_FLAG_DEFAULT_TO_DEVICE_PROTECTED_STORAGE, expected);
        flag("playback capture", info.privateFlags,
                ApplicationInfo.PRIVATE_FLAG_ALLOW_AUDIO_PLAYBACK_CAPTURE, expected);
        flag("save-state policy", info.privateFlags, ApplicationInfo.PRIVATE_FLAG_CANT_SAVE_STATE, expected);
        flag("resizeable", info.privateFlags,
                ApplicationInfo.PRIVATE_FLAG_ACTIVITIES_RESIZE_MODE_RESIZEABLE, expected);
        flag("unresizeable", info.privateFlags,
                ApplicationInfo.PRIVATE_FLAG_ACTIVITIES_RESIZE_MODE_UNRESIZEABLE, !expected);
        flag("back invoked", info.privateFlagsExt,
                ApplicationInfo.PRIVATE_FLAG_EXT_ENABLE_ON_BACK_INVOKED_CALLBACK, expected);
        flag("query-filtered attribution visibility", info.privateFlagsExt,
                ApplicationInfo.PRIVATE_FLAG_EXT_ATTRIBUTIONS_ARE_USER_VISIBLE, false);
    }

    public static void main(String[] args) throws Exception {
        Field singleton = Unsafe.class.getDeclaredField("theUnsafe");
        singleton.setAccessible(true);
        Unsafe unsafe = (Unsafe) singleton.get(null);
        manifest(apply(unsafe, parsed(true), true, false), true);
        manifest(apply(unsafe, parsed(false), true, false), false);
        // A declared hasCode flag must not undo the loader's no-DEX guard. Nor
        // may deriving ordinary manifest flags erase verified addon state.
        manifest(apply(unsafe, parsed(true), false, true), true);
        System.out.println("PackageManifestFlagsTest all passed: " + checks + " checks");
    }
}
