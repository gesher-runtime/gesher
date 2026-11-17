/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
import android.app.LoadedApk;
import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.content.res.CompatibilityInfo;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.gesher.server.ApkPackage;
import com.gesher.server.SharedLibraryGraph;
import java.io.File;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import sun.misc.Unsafe;

/** Actual metadata -> LoadedApk -> ContextImpl file operations, also runnable on baseline jars. */
public final class AppStoragePublicHost {
    private static int checks;
    private static void check(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
        checks++;
    }
    private static void set(Object object, String name, Object value) throws Exception {
        Field field = object.getClass().getDeclaredField(name);
        field.setAccessible(true); field.set(object, value);
    }
    private static Context context(Unsafe unsafe, LoadedApk loaded, int flags) throws Exception {
        // The host cannot bootstrap Android's ActivityThread/resources. Only those
        // constructors are bypassed; path selection and file APIs are AOSP's methods.
        Context context = (Context) unsafe.allocateInstance(Class.forName("android.app.ContextImpl"));
        set(context, "mPackageInfo", loaded); set(context, "mFlags", flags);
        set(context, "mFilesDirLock", new Object());
        return context;
    }
    private static String read(Context context) throws Exception {
        try (var input = context.openFileInput("same")) {
            return new String(input.readAllBytes(), StandardCharsets.UTF_8);
        }
    }
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        Field singleton = Unsafe.class.getDeclaredField("theUnsafe");
        singleton.setAccessible(true); Unsafe unsafe = (Unsafe) singleton.get(null);
        Path root = Files.createTempDirectory("gesher-public-storage-");
        try {
            for (boolean[] scenario : new boolean[][] {
                    {false, false}, {false, true}, {true, false}, {true, true}}) {
                boolean system = scenario[0], requestedDeviceDefault = scenario[1];
                boolean deviceDefault = system && requestedDeviceDefault;
                String name = "fixture.storage.public" + system + requestedDeviceDefault;
                String source = args[1];
                PackageImpl parsed = (PackageImpl) PackageImpl.forTesting(name, source);
                parsed.setTargetSdkVersion(36);
                parsed.setDefaultToDeviceProtectedStorage(requestedDeviceDefault);
                parsed.hideAsFinal();
                ApkPackage pkg = (ApkPackage) unsafe.allocateInstance(ApkPackage.class);
                set(pkg, "parsed", parsed); set(pkg, "packageName", name);
                set(pkg, "apkPath", source); set(pkg, "mUid", 10000);
                set(pkg, "sharedLibraries", SharedLibraryGraph.EMPTY);
                if (system) {
                    Method promote = ApkPackage.class.getDeclaredMethod("setSystemAddon", boolean.class);
                    promote.setAccessible(true);
                    promote.invoke(pkg, true); // Actual APK signature verification, no trusted-state override.
                }
                pkg.setDataDir(root.resolve(name).toString());
                ApplicationInfo info = pkg.applicationInfo(0);
                check(((info.privateFlags & ApplicationInfo.PRIVATE_FLAG_DEFAULT_TO_DEVICE_PROTECTED_STORAGE)
                                != 0) == deviceDefault,
                        "non-system manifest request acquired a device-protected default");
                Files.createDirectories(Path.of(info.credentialProtectedDataDir, "files"));
                Files.createDirectories(Path.of(info.deviceProtectedDataDir, "files"));
                LoadedApk loaded = new LoadedApk(null, info, CompatibilityInfo.DEFAULT_COMPATIBILITY_INFO,
                        null, false, false, false);
                Context ce = context(unsafe, loaded, Context.CONTEXT_CREDENTIAL_PROTECTED_STORAGE);
                Context de = context(unsafe, loaded, Context.CONTEXT_DEVICE_PROTECTED_STORAGE);
                check(!ce.getDataDir().getCanonicalFile().equals(de.getDataDir().getCanonicalFile()),
                        "REGRESSION: public ContextImpl credential/device storage aliases the same directory");
                check(new File(info.dataDir).equals(deviceDefault ? de.getDataDir() : ce.getDataDir()),
                        "manifest default storage disagrees with public ContextImpl");
                try (var output = ce.openFileOutput("same", Context.MODE_PRIVATE)) {
                    output.write("credential".getBytes(StandardCharsets.UTF_8));
                }
                try (var output = de.openFileOutput("same", Context.MODE_PRIVATE)) {
                    output.write("device".getBytes(StandardCharsets.UTF_8));
                }
                check("credential".equals(read(ce)) && "device".equals(read(de)),
                        "public file writes crossed storage areas");
                LoadedApk replay = new LoadedApk(null, info, CompatibilityInfo.DEFAULT_COMPATIBILITY_INFO,
                        null, false, false, false);
                check("credential".equals(read(context(unsafe, replay,
                                Context.CONTEXT_CREDENTIAL_PROTECTED_STORAGE)))
                                && "device".equals(read(context(unsafe, replay,
                                        Context.CONTEXT_DEVICE_PROTECTED_STORAGE))),
                        "fresh LoadedApk/ContextImpl lost independent files");
            }
            System.out.println("AppStoragePublicHost all passed: " + checks + " checks");
            System.out.println("Actual ApkPackage, LoadedApk and ContextImpl methods; host JNI boundaries and constructed contexts. No ART/bootstrap/SQLite claim.");
        } finally {
            try (var paths = Files.walk(root)) {
                for (Path path : paths.sorted(java.util.Comparator.reverseOrder()).toList()) {
                    Files.deleteIfExists(path);
                }
            }
        }
    }
}
