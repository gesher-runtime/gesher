/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
public final class IntentChooserHost {
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        android.app.PropertyInvalidatedCache.setTestMode(true);
        Class<?> locales = Class.forName("com.gesher.app.locale.SystemLocale");
        var hour = locales.getDeclaredField("sIs24Hour"); hour.setAccessible(true); hour.set(null, Boolean.TRUE);
        var list = locales.getDeclaredField("sLocales"); list.setAccessible(true);
        list.set(null, new android.os.LocaleList(java.util.Locale.ROOT));
        System.out.println("IntentChooser host all passed: " + gesher.services.ChooserContractTest.run(false) + " checks");
        System.out.println("Actual ATM/PM resolution, launches, lifecycle transactions, grants and PendingIntent callback; native identity and user input are host boundaries. Windows rendering/ART transport are separate checks.");
    }
}
