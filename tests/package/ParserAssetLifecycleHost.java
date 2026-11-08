/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
import android.content.pm.SigningDetails;
import android.content.pm.parsing.ApkLite;
import android.content.pm.parsing.PackageLite;
import android.content.res.ApkAssets;
import android.content.res.AssetManager;
import android.util.ArraySet;
import com.android.internal.pm.split.DefaultSplitAssetLoader;
import com.android.internal.pm.split.SplitAssetDependencyLoader;
import com.android.internal.pm.split.SplitAssetLoader;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.net.URLClassLoader;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Collections;
import java.util.List;
import java.util.Set;

/** Real framework Java loaders with a host-only mapping/JNI ownership ledger. */
public final class ParserAssetLifecycleHost {
    private static native int liveAssets();
    private static native int liveManagers();
    private static native void failSetup(int stage);
    private static int checks;

    private static void require(boolean value, String why) {
        if (!value) throw new AssertionError(why);
        checks++;
    }

    private static Field field(Class<?> type, String name) throws Exception {
        Field value = type.getDeclaredField(name); value.setAccessible(true); return value;
    }

    private static ApkLite metadata(String path) throws Exception {
        // Construct actual PackageLite inputs without claiming to parse a binary manifest on
        // HotSpot. The Windows entry point parses the real fixture APKs with ApkLiteParseUtils.
        Constructor<?> constructor = null;
        for (Constructor<?> candidate : ApkLite.class.getConstructors()) {
            if (candidate.getParameterCount() > 10) constructor = candidate;
        }
        if (constructor == null) throw new AssertionError("Pinned ApkLite constructor missing");
        Class<?>[] types = constructor.getParameterTypes();
        Object[] values = new Object[types.length];
        for (int i = 0; i < types.length; i++) {
            if (types[i] == boolean.class) values[i] = false;
            else if (types[i] == int.class) values[i] = 0;
            else if (types[i] == long.class) values[i] = 0L;
            else if (types[i] == List.class) values[i] = Collections.emptyList();
            else if (types[i] == Set.class) values[i] = Collections.emptySet();
            else if (types[i] == SigningDetails.class) values[i] = SigningDetails.UNKNOWN;
        }
        values[0] = path; values[1] = "com.gesher.splitcontract";
        return (ApkLite) constructor.newInstance(values);
    }

    private static void system(Path directory) throws Exception {
        Path source = directory.resolve("framework-res.apk");
        Files.write(source, new byte[]{'P', 'K', 3, 4, 5, 6, 7, 8});
        ApkAssets system = ApkAssets.loadFromPath(source.toString(), ApkAssets.PROPERTY_SYSTEM);
        field(AssetManager.class, "sSystemApkAssets").set(null, new ApkAssets[]{system});
        ArraySet<ApkAssets> set = new ArraySet<>(); set.add(system);
        field(AssetManager.class, "sSystemApkAssetsSet").set(null, set);
        Constructor<AssetManager> constructor = AssetManager.class.getDeclaredConstructor(boolean.class);
        constructor.setAccessible(true);
        AssetManager manager = constructor.newInstance(true);
        manager.setApkAssets(new ApkAssets[]{system}, false);
        field(AssetManager.class, "sSystem").set(null, manager);
    }

    private static void setupFailures(PackageLite pkg) throws Exception {
        int assetsBefore = liveAssets(), managersBefore = liveManagers();
        for (boolean isolated : new boolean[]{false, true}) {
            for (int stage : new int[]{1, 2}) {
                SplitAssetLoader loader = isolated ? new SplitAssetDependencyLoader(pkg,
                        SplitAssetDependencyLoader.createDependenciesFromPackage(pkg), 0)
                        : new DefaultSplitAssetLoader(pkg, 0);
                failSetup(stage);
                try {
                    loader.getBaseAssetManager();
                    throw new AssertionError("Injected manager setup failure was swallowed");
                } catch (IllegalStateException expected) {
                    require(expected.getMessage().equals("host manager setup failure " + stage),
                            "Loader changed the original manager setup exception");
                }
                require(liveManagers() == managersBefore,
                        "Unpublished native manager survived its setup failure");
                require(liveAssets() > assetsBefore, "Failure did not acquire private assets");
                @SuppressWarnings("unchecked")
                List<ApkAssets> retained = List.copyOf((List<ApkAssets>)
                        field(loader.getClass(), "mOwnedApkAssets").get(loader));
                loader.close(); loader.close();
                require(liveAssets() == assetsBefore, "Setup failure leaked private mappings");
                for (ApkAssets asset : retained) {
                    require(field(ApkAssets.class, "mNativePtr").getLong(asset) == 0,
                            "Failed loader's strongly retained private asset stayed open");
                }
            }
        }
    }

    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        Path directory = Path.of(args[2]);
        system(directory);
        String[] paths = new String[3];
        for (int i = 0; i < paths.length; i++) {
            Path path = directory.resolve(new String[]{"base.apk", "core.apk", "entry.apk"}[i]);
            Files.write(path, new byte[]{'P', 'K', 3, 4, 5, 6, 7, 8});
            paths[i] = path.toString();
        }
        ApkLite base = metadata(paths[0]);
        PackageLite pkg = new PackageLite(directory.toString(), paths[0], base,
                new String[]{"feature.core", "feature.entry"}, new boolean[]{true, true},
                new String[]{null, "feature.core"}, new String[]{null, null},
                new String[]{paths[1], paths[2]}, new int[]{0, 0}, 36, null, null);
        int ownedChecks;
        // Match ART's separately loaded fixture/boot-framework boundary.
        try (URLClassLoader child = new URLClassLoader(new java.net.URL[]{Path.of(args[1]).toUri().toURL()},
                ParserAssetLifecycleHost.class.getClassLoader())) {
            try {
                ownedChecks = (Integer) child.loadClass("gesher.services.ParserAssetLifecycleTest")
                        .getMethod("runOwnership", ApkLite.class, PackageLite.class, boolean.class)
                        .invoke(null, base, pkg, false);
            } catch (InvocationTargetException e) {
                throw new AssertionError("Actual loader ownership contract failed", e.getCause());
            }
        }
        require(liveAssets() == 1 && liveManagers() == 1,
                "Successful/partial loads retained a private host resource");
        setupFailures(pkg);
        require(liveAssets() == 1 && liveManagers() == 1, "Borrowed system ownership changed");
        System.out.println("ParserAssetLifecycleHost all passed: " + (ownedChecks + checks)
                + " (" + ownedChecks + " shared ownership, " + checks + " host failure/liveness)");
    }
}
