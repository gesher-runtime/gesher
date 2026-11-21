/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
public final class NotificationDelegationHost {
    public static void main(String[] args) throws Throwable {
        System.load(args[0]);
        String name = "gesher.services.NotificationDelegationTest";
        try (var loader = new java.net.URLClassLoader(
                new java.net.URL[]{NotificationDelegationHost.class.getProtectionDomain().getCodeSource().getLocation()},
                NotificationDelegationHost.class.getClassLoader()) {
            @Override protected Class<?> loadClass(String requested, boolean resolve) throws ClassNotFoundException {
                String context = "gesher.services.FixtureServiceContext";
                if (!requested.equals(name) && !requested.startsWith(name + "$")
                        && !requested.equals(context) && !requested.startsWith(context + "$")) {
                    return super.loadClass(requested, resolve);
                }
                synchronized (getClassLoadingLock(requested)) {
                    Class<?> loaded = findLoadedClass(requested);
                    if (loaded == null) loaded = findClass(requested);
                    if (resolve) resolveClass(loaded);
                    return loaded;
                }
            }
        }) {
            Class<?> fixture = Class.forName(name, true, loader);
            if (fixture.getClassLoader() == com.gesher.server.NotificationManagerService.class.getClassLoader()) {
                throw new AssertionError("Fixture must have a separate runtime package");
            }
            try {
                boolean storage = Boolean.getBoolean("gesher.fixture.notification.storage");
                int count = (int)fixture.getMethod("run", boolean.class, boolean.class, String.class)
                        .invoke(null, false, storage, args.length > 1 ? args[1] : null);
                System.out.println("Notification delegation host passed: " + count + " checks; protected="
                        + com.gesher.server.GmsSignatureSpoofing.isEnabled() + "; hostStorage=" + storage);
            } catch (java.lang.reflect.InvocationTargetException failure) { throw failure.getCause(); }
        }
        System.out.println("Production Java/CallerIdentity/PermissionPolicy/AtomicFile; constructed metadata/native identity boundaries. No ART, Binder transport or Windows delivery claim.");
    }
}
