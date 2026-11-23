/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
public final class ClipboardHost {
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        Class<?> locales = Class.forName("com.gesher.app.locale.SystemLocale");
        var hour = locales.getDeclaredField("sIs24Hour"); hour.setAccessible(true); hour.set(null, Boolean.TRUE);
        var list = locales.getDeclaredField("sLocales"); list.setAccessible(true);
        list.set(null, new android.os.LocaleList(java.util.Locale.ROOT));
        android.app.PropertyInvalidatedCache.setTestMode(true);
        android.app.PropertyInvalidatedCache.disableForTestMode();
        int security = gesher.services.ClipboardSecurityContractTest.run(false);
        int callbacks = gesher.services.ClipboardCallbackTest.contracts(false);
        System.out.println("Clipboard host all passed (GESHER_MICROG_SPOOF=" + System.getenv("GESHER_MICROG_SPOOF") + "): " + security + " policy and " + callbacks + " callback checks");
        System.out.println("Production PM/ATM/AppOps/Settings and clipboard policy with isolated native observations; Windows/ART and real focus boundaries are separate gates.");
    }
}
