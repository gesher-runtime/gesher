/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
public final class AppOpsHost {
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        // The host API archive has no ICU image. Locale initialization is unrelated
        // to AppOps and uses the same explicit boundary as the task-stack fixture.
        Class<?> locales = Class.forName("com.gesher.app.locale.SystemLocale");
        var hour = locales.getDeclaredField("sIs24Hour"); hour.setAccessible(true); hour.set(null, Boolean.TRUE);
        var list = locales.getDeclaredField("sLocales"); list.setAccessible(true);
        list.set(null, new android.os.LocaleList(java.util.Locale.ROOT));
        // Cross-process nonce storage is exercised by the ART wrapper. Host queries
        // run with Android's explicit cache test mode, leaving service policy intact.
        android.app.PropertyInvalidatedCache.setTestMode(true);
        android.app.PropertyInvalidatedCache.disableForTestMode();
        int checks = gesher.services.AppOpsContractTest.run(false);
        System.out.println("AppOps host all passed: " + checks + " checks");
        System.out.println("Real service/store/PermissionChecker and lifecycle code with constructed APK metadata and host native boundaries; ART and generated Binder Parcel validation run separately.");
    }
}
