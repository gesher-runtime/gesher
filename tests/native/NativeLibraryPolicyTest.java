/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.pm.PackageParser.PackageParserException;
import android.content.pm.SharedLibraryInfo;
import android.content.pm.VersionedPackage;
import com.android.internal.pm.parsing.pkg.ParsedPackage;
import com.gesher.server.CompatChanges;
import com.gesher.server.NativeLibraryCatalog;
import java.lang.reflect.Proxy;
import java.util.Arrays;
import java.util.List;

/** Production Java policy with real framework value objects and production JNI
 * catalog. ParsedPackage declarations are an explicit proxy input boundary;
 * real APK parsing/Installer execution is the separate ART gate. */
public final class NativeLibraryPolicyTest {
    private static int checks;
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        ++checks;
        System.out.println("PASS native Java policy: " + message);
    }
    private static ParsedPackage app(int sdk, List<String> required, List<String> optional) {
        return (ParsedPackage) Proxy.newProxyInstance(ParsedPackage.class.getClassLoader(),
                new Class<?>[]{ParsedPackage.class}, (proxy, method, args) -> switch (method.getName()) {
                    case "getPackageName" -> "gesher.namespace.policy.fixture";
                    case "getTargetSdkVersion" -> sdk;
                    case "getUsesNativeLibraries" -> required;
                    case "getUsesOptionalNativeLibraries" -> optional;
                    default -> throw new AssertionError("Unexpected parsed-package access " + method);
                });
    }
    private static void missing(String name, String abi) throws Exception {
        try {
            NativeLibraryCatalog.resolve(app(31, List.of(name), List.of()), abi);
        } catch (PackageParserException expected) {
            check(expected.error == PackageManager.INSTALL_FAILED_MISSING_SHARED_LIBRARY,
                    "required absence retains Android install failure for " + name + "/" + abi);
            return;
        }
        throw new AssertionError("Required absence succeeded");
    }
    public static void main(String[] ignored) throws Exception {
        long change = CompatChanges.ENFORCE_NATIVE_SHARED_LIBRARY_DEPENDENCIES;
        check(!CompatChanges.isEnabled(change, 30) && CompatChanges.isEnabled(change, 31)
                && Arrays.binarySearch(CompatChanges.disabledFor(30), change) >= 0
                && Arrays.binarySearch(CompatChanges.disabledFor(31), change) < 0,
                "PM native enforcement and bindApplication use the same SDK30/31 compat policy");
        NativeLibraryCatalog catalog = NativeLibraryCatalog.readSystem();
        List<String> names = Arrays.asList(catalog.names());
        check(names.contains("libregistry_device.so") && names.contains("libpresent.fixture.so")
                && !names.contains("libregistry_ndk.so") && !names.contains("libregistry_private.so"),
                "Java device declarations come from actual device providers/files, excluding NDK/private rows");
        check(NativeLibraryCatalog.resolve(app(30, List.of("libabsent.so"), List.of()), "x86_64").isEmpty(),
                "SDK30 ignores the required native declaration under the real compat rule");
        missing("libabsent.so", "x86_64");
        missing("libregistry_private.so", "x86_64");
        missing("libregistry_device.so", "arm64-v8a");
        check(NativeLibraryCatalog.resolve(app(31, List.of(), List.of("libabsent.so")), "x86_64").isEmpty(),
                "SDK31 optional absence remains an installation-compatible empty allowlist");
        List<SharedLibraryInfo> selected = NativeLibraryCatalog.resolve(app(31,
                List.of("libregistry_device.so"), List.of("libpresent.fixture.so", "libabsent.so")), "x86_64");
        check(selected.size() == 2 && selected.stream().allMatch(SharedLibraryInfo::isNative),
                "required/optional present native providers produce native SharedLibraryInfo objects");
        ApplicationInfo info = new ApplicationInfo();
        String[] dexFiles = {"fixture-java-library.jar"};
        SharedLibraryInfo javaLibrary = new SharedLibraryInfo(dexFiles[0], null, null, "fixture.java",
                SharedLibraryInfo.VERSION_UNDEFINED, SharedLibraryInfo.TYPE_BUILTIN,
                new VersionedPackage("android", 0), null, null, false);
        info.sharedLibraryInfos = List.of(javaLibrary);
        info.sharedLibraryFiles = dexFiles;
        NativeLibraryCatalog.apply(info, selected);
        check(info.sharedLibraryFiles == dexFiles && info.sharedLibraryInfos.size() == 3
                && info.sharedLibraryInfos.get(0) == javaLibrary
                && info.sharedLibraryInfos.get(1).isNative() && info.sharedLibraryInfos.get(2).isNative(),
                "native metadata append preserves Java graph objects and the exact dex-file array");
        System.out.println("NativeLibraryPolicyTest all passed: " + checks + " production Java/JNI policy checks");
    }
}
