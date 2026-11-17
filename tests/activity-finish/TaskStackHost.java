/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
public final class TaskStackHost {
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        // The host API archive has no ICU image. These unrelated locale settings
        // do not replace activity/task/PendingIntent code or lifecycle decisions.
        Class<?> locales = Class.forName("com.gesher.app.locale.SystemLocale");
        var hour = locales.getDeclaredField("sIs24Hour"); hour.setAccessible(true); hour.set(null, Boolean.TRUE);
        var list = locales.getDeclaredField("sLocales"); list.setAccessible(true);
        list.set(null, new android.os.LocaleList(java.util.Locale.ROOT));
        int checks = gesher.services.TaskStackContractTest.run(false, args[1]);
        System.out.println("TaskStack host all passed: " + checks + " checks");
        System.out.println("Actual production task/creator dispatch, public framework TaskStackBuilder/PendingIntent and transaction objects; declared host identity boundaries. No ART, Parcel transport or window execution claim.");
    }
}
