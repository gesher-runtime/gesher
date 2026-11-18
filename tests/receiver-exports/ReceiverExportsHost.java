/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
public final class ReceiverExportsHost {
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        android.app.PropertyInvalidatedCache.setTestMode(true);
        android.app.PropertyInvalidatedCache.disableForTestMode();
        // Host initialization only: cached classes do not include Android's ICU data.
        var hour = Class.forName("com.gesher.app.locale.SystemLocale").getDeclaredField("sIs24Hour");
        hour.setAccessible(true); hour.set(null, Boolean.TRUE);
        System.out.println("Receiver export host passed: "
                + gesher.services.ReceiverExportContractTest.run(false) + " checks");
    }
}
