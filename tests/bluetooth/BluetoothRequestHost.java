/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
public final class BluetoothRequestHost {
    public static void main(String[] args) throws Throwable {
        System.load(args[0]);
        String fixtureName = "com.gesher.server.BluetoothRequestFixture";
        try (var loader = new java.net.URLClassLoader(
                new java.net.URL[]{BluetoothRequestHost.class.getProtectionDomain().getCodeSource().getLocation()},
                BluetoothRequestHost.class.getClassLoader()) {
            @Override protected Class<?> loadClass(String name, boolean resolve) throws ClassNotFoundException {
                if (!name.equals(fixtureName) && !name.startsWith(fixtureName + "$")) return super.loadClass(name, resolve);
                synchronized (getClassLoadingLock(name)) {
                    Class<?> found = findLoadedClass(name);
                    if (found == null) found = findClass(name);
                    if (resolve) resolveClass(found);
                    return found;
                }
            }
        }) {
            Class<?> fixture = Class.forName(fixtureName, true, loader);
            if (fixture.getClassLoader() == com.gesher.server.ActivityTaskManagerService.class.getClassLoader())
                throw new AssertionError("Fixture must not share the production runtime package");
            int checks;
            try { checks = (int) fixture.getMethod("run").invoke(null); }
            catch (java.lang.reflect.InvocationTargetException failure) { throw failure.getCause(); }
            System.out.println("Bluetooth request host all passed: " + checks + " checks");
            System.out.println("Actual Bluetooth request/ATM Java and permission policy; isolated manifests, metadata, radio, clock and native initialization. No Windows radio or ART claim.");
        }
    }
}
