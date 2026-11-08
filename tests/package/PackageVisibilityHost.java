/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
public final class PackageVisibilityHost {
    public static void main(String[] args) throws Throwable {
        System.load(args[0]);
        String name = "gesher.services.PackageVisibilityContractTest";
        try (var loader = new java.net.URLClassLoader(
                new java.net.URL[]{PackageVisibilityHost.class.getProtectionDomain().getCodeSource().getLocation()},
                PackageVisibilityHost.class.getClassLoader()) {
            @Override protected Class<?> loadClass(String value, boolean resolve) throws ClassNotFoundException {
                if (!value.equals(name) && !value.startsWith(name + "$")) return super.loadClass(value, resolve);
                synchronized (getClassLoadingLock(value)) {
                    Class<?> result = findLoadedClass(value);
                    if (result == null) result = findClass(value);
                    if (resolve) resolveClass(result); return result;
                }
            }
        }) {
            Class<?> fixture = Class.forName(name, true, loader);
            if (fixture.getClassLoader() == com.gesher.server.PackageManagerService.class.getClassLoader()) {
                throw new AssertionError("Fixture must use a separate runtime loader");
            }
            int checks;
            try { checks = (int)fixture.getMethod("run", boolean.class).invoke(null, false); }
            catch (java.lang.reflect.InvocationTargetException failure) { throw failure.getCause(); }
            System.out.println("Package visibility host all passed: " + checks + " checks; protected="
                    + com.gesher.server.GmsSignatureSpoofing.isEnabled());
            System.out.println("Actual PMS/framework/APM; constructed parsed manifests, controlled native identity and framework-catalog boundary. No ART/Binder transport or APK parsing claim.");
        }
    }
}
