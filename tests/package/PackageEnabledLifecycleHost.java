/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
public final class PackageEnabledLifecycleHost {
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        android.app.PropertyInvalidatedCache.setTestMode(true);
        var field = android.content.pm.PackageManager.class.getDeclaredField("sPackageInfoCache");
        field.setAccessible(true);
        ((android.app.PropertyInvalidatedCache<?, ?>)field.get(null)).testPropertyName();
        // Constructed locale setting; the host does not ship Android's ICU data bundle.
        var hour = Class.forName("com.gesher.app.locale.SystemLocale").getDeclaredField("sIs24Hour");
        hour.setAccessible(true); hour.set(null, Boolean.TRUE);
        String fixtureName = "com.gesher.server.PackageEnabledLifecycleFixture";
        int checks;
        // ART loads fixture dex separately from bootclasspath production classes.
        // Mirror that boundary here so same-package source does not hide illegal access.
        try (var loader = new java.net.URLClassLoader(
                new java.net.URL[]{PackageEnabledLifecycleHost.class.getProtectionDomain().getCodeSource().getLocation()},
                PackageEnabledLifecycleHost.class.getClassLoader()) {
            @Override protected Class<?> loadClass(String name, boolean resolve) throws ClassNotFoundException {
                if (!name.equals(fixtureName) && !name.startsWith(fixtureName + "$")) return super.loadClass(name, resolve);
                synchronized (getClassLoadingLock(name)) {
                    Class<?> loaded = findLoadedClass(name);
                    if (loaded == null) loaded = findClass(name);
                    if (resolve) resolveClass(loaded);
                    return loaded;
                }
            }
        }) {
            ClassLoader original = Thread.currentThread().getContextClassLoader();
            Thread.currentThread().setContextClassLoader(loader);
            try {
                Class<?> fixture = Class.forName(fixtureName, true, loader);
                if (fixture.getClassLoader() == com.gesher.server.ActivityTaskManagerService.class.getClassLoader())
                    throw new AssertionError("Fixture shared the production runtime package");
                checks = (int)fixture.getMethod("run", boolean.class, String.class).invoke(null, false, args[1]);
            } catch (java.lang.reflect.InvocationTargetException error) {
                if (error.getCause() instanceof Error failure) throw failure;
                if (error.getCause() instanceof Exception failure) throw failure;
                throw error;
            } finally { Thread.currentThread().setContextClassLoader(original); }
        }
        System.out.println("Package enabled lifecycle host passed: " + checks + " checks; group=" + args[1]);
        System.out.println("Actual PM/AM/controllers and attach dispatch; constructed metadata and recording native/process boundaries. No OS process control or ART transport claim.");
    }
}
