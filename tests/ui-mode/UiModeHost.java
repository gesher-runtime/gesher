/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
public final class UiModeHost {
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        Class<?> locales = Class.forName("com.gesher.app.locale.SystemLocale");
        var hour = locales.getDeclaredField("sIs24Hour"); hour.setAccessible(true); hour.set(null, Boolean.TRUE);
        var list = locales.getDeclaredField("sLocales"); list.setAccessible(true);
        list.set(null, new android.os.LocaleList(java.util.Locale.ROOT));
        android.app.PropertyInvalidatedCache.setTestMode(true);
        android.app.PropertyInvalidatedCache.disableForTestMode();
        // ART loads gesher-app.jar from the boot class path and services-tests.dex
        // separately. A matching package name does not grant package access across
        // defining loaders. Keep that boundary in the host regression as well.
        try (var loader = new java.net.URLClassLoader(new java.net.URL[]{
                new java.io.File(args[1]).toURI().toURL()}, UiModeHost.class.getClassLoader())) {
            Class<?> fixture = Class.forName("com.gesher.server.UiModeFixture", true, loader);
            if (fixture.getClassLoader() == com.gesher.server.UiModeManagerService.class.getClassLoader()) {
                throw new AssertionError("UiMode fixture and production classes share a defining loader");
            }
            if (args.length == 3) {
                fixture.getMethod("prepareShutdown", java.io.File.class).invoke(null, new java.io.File(args[2]));
                System.exit(0);
            }
            int checks = (Integer) fixture.getMethod("run", boolean.class).invoke(null, false);
            System.out.println("UiMode host all passed: " + checks + " checks; separate fixture loader");
        }
        System.out.println("Production UiMode/PM/ATM configuration lifecycle and store; constructed package metadata, explicit host inputs, JNI identity boundary. Generated Binder Parcel and actual Windows events run separately on ART.");
    }
}
