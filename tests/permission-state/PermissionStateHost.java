/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
public final class PermissionStateHost {
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        Class<?> locales = Class.forName("com.gesher.app.locale.SystemLocale");
        var hour = locales.getDeclaredField("sIs24Hour"); hour.setAccessible(true); hour.set(null, Boolean.TRUE);
        var list = locales.getDeclaredField("sLocales"); list.setAccessible(true);
        list.set(null, new android.os.LocaleList(java.util.Locale.ROOT));
        android.app.PropertyInvalidatedCache.setTestMode(true);
        android.app.PropertyInvalidatedCache.disableForTestMode();
        int checks = gesher.services.RuntimePermissionStateTest.run(false);
        System.out.println("Runtime permission host all passed: " + checks + " checks");
        System.out.println("Production permission controller/policy/store; constructed APK metadata and host native identity. Windows UI and ART Binder transport are separate tests.");
    }
}
