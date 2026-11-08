/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
public final class ComponentCallerHost {
    public static void main(String[] args) throws Throwable {
        System.load(args[0]);
        String fixtureName = "com.gesher.server.ComponentCallerFixture";
        // ART loads services-tests.dex separately from the boot services. Matching
        // package names are not runtime package access: reproduce that boundary.
        try (var fixtureLoader = new java.net.URLClassLoader(
                new java.net.URL[]{ComponentCallerHost.class.getProtectionDomain().getCodeSource().getLocation()},
                ComponentCallerHost.class.getClassLoader()) {
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
            Class<?> fixture = Class.forName(fixtureName, true, fixtureLoader);
            if (fixture.getClassLoader() == com.gesher.server.ActivityTaskManagerService.class.getClassLoader()) {
                throw new AssertionError("Fixture must not share the production runtime package");
            }
            int checks;
            try { checks = (int)fixture.getMethod("run", boolean.class).invoke(null, false); }
            catch (java.lang.reflect.InvocationTargetException failure) { throw failure.getCause(); }
            System.out.println("ComponentCaller host all passed: " + checks + " checks");
            System.out.println("Separate fixture/service class loaders; real public caller/controller/provider policy; constructed manifest/process state and controlled native identity. No ART, Binder transport or actual Activity callback claim.");
        }
    }
}
