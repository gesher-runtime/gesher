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
        int checks = com.gesher.server.UiModeFixture.run(false);
        System.out.println("UiMode host all passed: " + checks + " checks");
        System.out.println("Production UiMode/PM/ATM configuration lifecycle and store; constructed package metadata, explicit host inputs, JNI identity boundary. Generated Binder Parcel and actual Windows events run separately on ART.");
    }
}
