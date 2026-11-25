/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
public final class ForegroundServiceAdmissionHost {
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        var hour = Class.forName("com.gesher.app.locale.SystemLocale").getDeclaredField("sIs24Hour");
        hour.setAccessible(true); hour.set(null, Boolean.TRUE);
        android.app.PropertyInvalidatedCache.setTestMode(true);
        android.app.PropertyInvalidatedCache.disableForTestMode();
        int count = gesher.services.ForegroundServiceAdmissionTest.run(false, args.length > 1 ? args[1] : "all");
        System.out.println("Foreground service admission host all passed: " + count + " checks");
        System.out.println("Production AM/ActiveServices/PermissionPolicy, constructed APK metadata and host JNI boundaries. No ART, Parcel or Android reference execution claim.");
    }
}
