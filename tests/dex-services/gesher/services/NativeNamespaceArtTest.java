/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.system.Os;
import com.android.internal.os.ClassLoaderFactory;
import dalvik.system.VMRuntime;
import java.lang.reflect.InvocationTargetException;
import java.util.List;

/** The real ART System.load/JNI_OnLoad/ClassLoaderFactory gate. Run only on the shipped runtime. */
public final class NativeNamespaceArtTest {
    private static int checks;
    private static void check(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
        ++checks;
        System.out.println("PASS ART native namespace: " + reason);
    }
    private static Class<?> probe(String root, String directory, int sdk, List<String> nativeLibraries)
            throws Exception {
        ClassLoader parent = ClassLoader.getSystemClassLoader().getParent();
        ClassLoader loader = ClassLoaderFactory.createClassLoader(root + "/native-payload.jar",
                root + "/" + directory, root + "/" + directory, parent, sdk,
                false, null, null, nativeLibraries, null);
        return Class.forName("gesher.nativepayload.Probe", true, loader);
    }
    private static void load(Class<?> probe, String path) throws Exception {
        probe.getMethod("load", String.class).invoke(null, path);
    }
    private static int value(Class<?> probe, String method) throws Exception {
        return (Integer) probe.getMethod(method).invoke(null);
    }
    private static void rejects(Class<?> probe, String method, String library) throws Exception {
        try {
            probe.getMethod(method, String.class).invoke(null, library);
        } catch (InvocationTargetException failure) {
            check(failure.getCause() instanceof UnsatisfiedLinkError,
                    "native load retains UnsatisfiedLinkError for " + library);
            return;
        }
        throw new AssertionError("Native load unexpectedly succeeded: " + library);
    }
    public static void main(String[] args) throws Exception {
        Class.forName("com.gesher.app.Main").getMethod("bootstrap", String.class,
                String.class, String.class).invoke(null, "native namespace", args[0], args[1]);
        String root = args[2];
        String oldRoot = System.getenv("ANDROID_ROOT");
        String oldPath = System.getProperty("java.library.path");
        VMRuntime runtime = VMRuntime.getRuntime();
        int originalTargetSdk = runtime.getTargetSdkVersion();
        try {
            runtime.setTargetSdkVersion(23);
            Os.setenv("ANDROID_ROOT", root + "/device/system", true);
            System.setProperty("java.library.path", root + "/device/system/lib64");
            // Run before any other application JNI load or catalog query.
            // The compiler records libnativehelper.so/LIBNATIVEHELPER_S.
            Class<?> helper = probe(root, "helper", 31, List.of());
            load(helper, root + "/helper/libjnihelper.so");
            check(value(helper, "helperVmIdentity") == 31,
                    "first app load resolves public nativehelper and preserves actual VM/JNIEnv identity");
            Class<?> alpha = probe(root, "alpha", 31, List.of());
            Class<?> beta = probe(root, "beta", 31, List.of());
            load(alpha, root + "/alpha/libjnicollision.so");
            load(beta, root + "/beta/libjnicollision.so");
            check(value(alpha, "applicationTargetSdk") == 23,
                    "actual VM target initializer reaches native libdl and classloader creation preserves it");
            runtime.setTargetSdkVersion(31);
            check(value(alpha, "applicationTargetSdk") == 31,
                    "an already-loaded JNI caller observes an actual VM target update");
            runtime.setTargetSdkVersion(0);
            check(value(alpha, "applicationTargetSdk") == android.os.Build.VERSION.SDK_INT,
                    "the native target setter maps zero to the shipped platform API");
            check(value(alpha, "value") == 100 && value(beta, "value") == 200,
                    "two ART classloaders own different same-SONAME JNI files");
            check(value(alpha, "constructors") == 1 && value(beta, "constructors") == 1
                    && value(alpha, "onloads") == 1 && value(beta, "onloads") == 1,
                    "ELF constructors and ART JNI_OnLoad run once for each original JNI path");
            load(alpha, root + "/alpha/./libjnicollision.so");
            check(value(alpha, "constructors") == 1,
                    "a same-file alias keeps ELF constructor/storage identity");
            check(value(alpha, "onloads") == 2,
                    "ART retains pinned path-based JNI_OnLoad semantics for an alias");
            Class<?> otherOwner = probe(root, "alpha", 31, List.of());
            rejects(otherOwner, "load", root + "/alpha/libjnicollision.so");
            // Check visibility before any other classloader owns this JNI path,
            // so ART's duplicate-path rejection cannot mask a namespace leak.
            Class<?> undeclared = probe(root, "alpha", 31, List.of());
            rejects(undeclared, "loadLibrary", "catalog.gesher");
            Class<?> declared = probe(root, "alpha", 31, List.of("libcatalog.gesher.so"));
            declared.getMethod("loadLibrary", String.class).invoke(null, "catalog.gesher");
            check(value(declared, "value") == 300,
                    "declared device provider reaches the actual ART native load path");
            System.out.println("NativeNamespaceArtTest all passed: " + checks + " real ART checks");
        } finally {
            runtime.setTargetSdkVersion(originalTargetSdk);
            if (oldRoot == null) Os.unsetenv("ANDROID_ROOT"); else Os.setenv("ANDROID_ROOT", oldRoot, true);
            if (oldPath == null) System.clearProperty("java.library.path");
            else System.setProperty("java.library.path", oldPath);
        }
    }
}
