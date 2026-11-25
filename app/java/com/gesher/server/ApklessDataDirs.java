package com.gesher.server;

import android.content.pm.ApplicationInfo;

import java.io.File;

/**
 * Data areas for a package Gesher describes without an APK (no {@link ApkPackage}).
 *
 * <p>It gets the data areas an installed package of that name has under
 * {@code $ANDROID_DATA}, as ApkPackage.applicationInfo does, and its default native
 * library directory. Nothing is created: another app that opens such a package's
 * context finds no writable private directory, as it does for any other package on
 * Android, and never reaches a path outside the device's data root. Kept apart from
 * {@link AppDataStorage}, which is compiled without Android classes by
 * tools/test_app_data_storage.py.
 */
final class ApklessDataDirs {
    private ApklessDataDirs() {}

    static void describe(ApplicationInfo ai) {
        AppDataStorage storage = AppDataStorage.installed(new File(Device.dataDir()), ai.packageName);
        ai.dataDir = storage.credentialDirectory();
        ai.credentialProtectedDataDir = ai.dataDir;
        ai.deviceProtectedDataDir = storage.deviceDirectory();
        ai.nativeLibraryDir = ai.dataDir + "/lib";
    }
}
