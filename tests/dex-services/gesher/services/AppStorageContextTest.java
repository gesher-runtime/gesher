/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.content.res.AssetManager;
import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;
import android.os.Looper;
import android.os.Process;
import android.os.ServiceManager;
import android.os.UserManager;
import android.os.storage.IStorageManager;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.gesher.server.ApkPackage;
import com.gesher.server.Services;
import com.gesher.server.SharedLibraryGraph;
import java.io.File;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.List;
import sun.misc.Unsafe;

/** Real ContextImpl files, SharedPreferences, SQLite and storage-context migration on ART. */
public final class AppStorageContextTest {
    private static int checks;

    private static void check(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
        checks++;
    }

    private static void set(Object object, String name, Object value) throws Exception {
        Field field = object.getClass().getDeclaredField(name);
        field.setAccessible(true); field.set(object, value);
    }

    private static ApkPackage record(Unsafe unsafe, String name, File data,
            boolean requestedDeviceDefault, boolean system)
            throws Exception {
        // This fixture does not replace ContextImpl, LoadedApk, preferences or SQLite.
        // The native APK parser and completed system scan are controlled inputs;
        // metadata comes from production ApkPackage and AOSP's PackageInfo generator.
        // AppStoragePolicyHost separately exercises real signed-APK promotion.
        String source = System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk";
        PackageImpl parsed = (PackageImpl) PackageImpl.forTesting(name, source);
        parsed.setTargetSdkVersion(36);
        parsed.setEnabled(true);
        parsed.setDirectBootAware(true);
        parsed.setDefaultToDeviceProtectedStorage(requestedDeviceDefault);
        ApkPackage apk = (ApkPackage) unsafe.allocateInstance(ApkPackage.class);
        set(apk, "packageName", name); set(apk, "parsed", parsed);
        set(apk, "apkPath", source); set(apk, "mUid", Process.myUid());
        set(apk, "mSystemAddon", system);
        set(apk, "sharedLibraries", SharedLibraryGraph.EMPTY);
        apk.setDataDir(new File(data, name).getPath());
        return apk;
    }

    private static void legacyMigration(Unsafe unsafe, File data) throws Exception {
        String name = "fixture.storage.legacy";
        ApkPackage legacy = record(unsafe, name, data, true, true);
        File credential = new File(legacy.credentialProtectedDataDir());
        File device = new File(legacy.deviceProtectedDataDir());
        Files.createDirectories(new File(credential, "files").toPath());
        Files.writeString(new File(credential, "files/old").toPath(), "old default data");
        Method prepare = ApkPackage.class.getDeclaredMethod("prepareInstalledDataDirectories");
        prepare.setAccessible(true);
        // Real ART FileChannel locks, synced atomic journal replacement and directory rename.
        prepare.invoke(legacy);
        check(!new File(credential, "files/old").exists()
                        && "old default data".equals(Files.readString(new File(device, "files/old").toPath()))
                        && credential.isDirectory(),
                "Actual installed preparation did not preserve and separate legacy default data");
        Files.createDirectories(new File(credential, "files").toPath());
        Files.writeString(new File(credential, "files/new").toPath(), "new credential data");
        ApkPackage restarted = record(unsafe, name, data, true, true);
        prepare.invoke(restarted);
        check("new credential data".equals(Files.readString(new File(credential, "files/new").toPath()))
                        && !new File(device, "files/new").exists()
                        && "old default data".equals(Files.readString(new File(device, "files/old").toPath())),
                "Actual restart repeated legacy migration or lost an independent storage area");
    }

    private static void write(Context context, String name, String value) throws Exception {
        try (var output = context.openFileOutput(name, Context.MODE_PRIVATE)) {
            output.write(value.getBytes(StandardCharsets.UTF_8));
        }
    }

    private static String read(Context context, String name) throws Exception {
        try (var input = context.openFileInput(name)) {
            return new String(input.readAllBytes(), StandardCharsets.UTF_8);
        }
    }

    private static void exercise(Context context, ApkPackage apk, boolean deviceDefault) throws Exception {
        Context ce = context.createCredentialProtectedStorageContext();
        Context de = context.createDeviceProtectedStorageContext();
        ApplicationInfo metadata = apk.applicationInfo(0);
        check(context.isDeviceProtectedStorage() == deviceDefault
                        && context.isCredentialProtectedStorage() != deviceDefault,
                "manifest default does not select the matching Context storage mode");
        check(ce.isCredentialProtectedStorage() && !ce.isDeviceProtectedStorage()
                        && de.isDeviceProtectedStorage() && !de.isCredentialProtectedStorage(),
                "explicit context storage modes are inconsistent");
        check(!ce.getDataDir().getCanonicalFile().equals(de.getDataDir().getCanonicalFile())
                        && ce.getDataDir().equals(new File(metadata.credentialProtectedDataDir))
                        && de.getDataDir().equals(new File(metadata.deviceProtectedDataDir))
                        && context.getDataDir().equals(deviceDefault ? de.getDataDir() : ce.getDataDir()),
                "actual ContextImpl paths alias or disagree with application metadata");
        check(context.getPackageManager().getApplicationInfo(apk.packageName, 0).dataDir
                        .equals(context.getDataDir().getPath()),
                "package query and running context disagree on the default area");

        write(ce, "same-name", "credential"); write(de, "same-name", "device");
        check("credential".equals(read(ce, "same-name"))
                        && "device".equals(read(de, "same-name")),
                "same filename crossed CE/DE contexts");
        check(ce.getSharedPreferences("same", 0).edit().putInt("value", 41).commit()
                        && de.getSharedPreferences("same", 0).edit().putInt("value", 82).commit(),
                "preferences failed to commit");
        check(ce.getSharedPreferences("same", 0).getInt("value", -1) == 41
                        && de.getSharedPreferences("same", 0).getInt("value", -1) == 82,
                "preferences cache or disk paths crossed storage contexts");
        check(ce.getSharedPreferences("move", 0).edit().putString("value", "moved").commit(),
                "source preferences did not commit");
        check(de.moveSharedPreferencesFrom(ce, "move")
                        && "moved".equals(de.getSharedPreferences("move", 0).getString("value", null))
                        && !new File(ce.getDataDir(), "shared_prefs/move.xml").exists(),
                "public preferences migration failed or left the source file");
        check(ce.moveSharedPreferencesFrom(de, "move")
                        && "moved".equals(ce.getSharedPreferences("move", 0).getString("value", null)),
                "reverse preferences migration failed");

        for (Context area : new Context[] {ce, de}) {
            try (SQLiteDatabase db = area.openOrCreateDatabase("same.db", 0, null)) {
                db.execSQL("CREATE TABLE values_table (value INTEGER)");
                db.execSQL("INSERT INTO values_table VALUES (?)", new Object[] {area == ce ? 17 : 29});
            }
        }
        check(databaseValue(ce, "same.db") == 17 && databaseValue(de, "same.db") == 29,
                "same database filename crossed storage contexts");
        try (SQLiteDatabase db = ce.openOrCreateDatabase("move.db", 0, null)) {
            db.execSQL("CREATE TABLE values_table (value INTEGER)");
            db.execSQL("INSERT INTO values_table VALUES (73)");
        }
        check(de.moveDatabaseFrom(ce, "move.db") && !ce.getDatabasePath("move.db").exists()
                        && databaseValue(de, "move.db") == 73,
                "public database migration lost the database or retained the source");
        check(ce.moveDatabaseFrom(de, "move.db") && databaseValue(ce, "move.db") == 73,
                "reverse database migration failed");
        check(context.getSystemService(UserManager.class).isUserUnlocked()
                        && IStorageManager.Stub.asInterface(ServiceManager.getService("mount"))
                                .isCeStorageUnlocked(0),
                "always-unlocked user and mount contracts disagree");

        Context recreated = MediaRouterContractTest.context(apk.packageName);
        check("credential".equals(read(recreated.createCredentialProtectedStorageContext(), "same-name"))
                        && "device".equals(read(recreated.createDeviceProtectedStorageContext(), "same-name"))
                        && databaseValue(recreated.createCredentialProtectedStorageContext(), "same.db") == 17
                        && databaseValue(recreated.createDeviceProtectedStorageContext(), "same.db") == 29,
                "new LoadedApk/Context lost independently persisted areas");
    }

    private static int databaseValue(Context context, String name) {
        try (SQLiteDatabase db = context.openOrCreateDatabase(name, 0, null);
                Cursor cursor = db.rawQuery("SELECT value FROM values_table", null)) {
            check(cursor.moveToFirst(), "persisted database row is missing");
            return cursor.getInt(0);
        }
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        com.android.internal.os.ApplicationSharedMemory.setInstance(
                com.android.internal.os.ApplicationSharedMemory.create());
        AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        if (Looper.myLooper() == null) Looper.prepareMainLooper();
        Field singleton = Unsafe.class.getDeclaredField("theUnsafe");
        singleton.setAccessible(true); Unsafe unsafe = (Unsafe) singleton.get(null);
        File data = Files.createTempDirectory("gesher-context-storage-").toFile();
        android.system.Os.setenv("ANDROID_DATA", data.getPath(), true);
        ApkPackage ordinary = record(unsafe, "fixture.storage.credential", data, false, false);
        ApkPackage requested = record(unsafe, "fixture.storage.requested", data, true, false);
        ApkPackage device = record(unsafe, "fixture.storage.device", data, true, true);
        ordinary.prepareDataDirectories(); requested.prepareDataDirectories(); device.prepareDataDirectories();
        legacyMigration(unsafe, data);
        Services.installInProcess(Services.createInProcess(List.of(ordinary, requested, device)));
        exercise(MediaRouterContractTest.context(ordinary.packageName), ordinary, false);
        exercise(MediaRouterContractTest.context(requested.packageName), requested, false);
        exercise(MediaRouterContractTest.context(device.packageName), device, true);
        System.out.println("AppStorageContextTest all passed: " + checks + " checks");
        System.exit(0);
    }
}
