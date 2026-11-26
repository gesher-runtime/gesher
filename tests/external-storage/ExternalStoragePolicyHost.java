/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
public final class ExternalStoragePolicyHost {
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        Class<?> locales = Class.forName("com.gesher.app.locale.SystemLocale");
        var hour = locales.getDeclaredField("sIs24Hour"); hour.setAccessible(true); hour.set(null, Boolean.TRUE);
        var list = locales.getDeclaredField("sLocales"); list.setAccessible(true);
        list.set(null, new android.os.LocaleList(java.util.Locale.ROOT));
        android.app.PropertyInvalidatedCache.setTestMode(true);
        android.app.PropertyInvalidatedCache.disableForTestMode();
        int checks = gesher.services.ExternalStoragePolicyContractTest.run(true);
        System.out.println("External storage policy host passed: " + checks + " checks");
        System.out.println("Real PM, permissions, AppOps, policy files and mount service; constructed APK metadata and existing host JNI identity boundary. File I/O is delayed or failed only in isolated fixtures. Windows ACLs and ART transport are separate gates.");
    }
}
