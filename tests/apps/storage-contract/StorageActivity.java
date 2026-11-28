/* SPDX-License-Identifier: Apache-2.0 */
package org.gesher.contracts.storage;

import android.app.Activity;
import android.content.Context;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;
import android.os.Bundle;
import android.os.Process;
import android.os.UserManager;
import android.util.Log;

import org.json.JSONObject;

import java.nio.charset.StandardCharsets;
import java.util.UUID;

public final class StorageActivity extends Activity {
    private static final String TAG = "GesherStorageContract";
    private static final String FILE = "shared-name.txt";
    private static final String PREFS = "shared-name";
    private static final String DB = "shared-name.db";
    private static final String MARKER = "storage-phase.txt";

    @Override
    public void onCreate(Bundle state) {
        super.onCreate(state);
        JSONObject report = new JSONObject();
        try {
            report.put("schema", 1);
            report.put("pid", Process.myPid());
            report.put("target_sdk", getApplicationInfo().targetSdkVersion);
            UserManager users = getSystemService(UserManager.class);
            if (users == null || !users.isUserUnlocked()) {
                report.put("status", "blocked");
                report.put("reason", "user_not_unlocked");
                return;
            }
            runStorageContract(report);
            DexContract.observe(this, report.getJSONObject("checks"));
            report.put("status", "observed");
        } catch (Exception | LinkageError failure) {
            Log.e(TAG, "Storage contract failed", failure);
            try {
                report.put("status", "error");
                report.put("reason", failure.getClass().getName());
            } catch (org.json.JSONException impossible) {
                throw new AssertionError(impossible);
            }
        } finally {
            String result = report.toString();
            System.out.println(TAG + " " + result);
            System.out.flush();
            Log.i(TAG, result);
            finish();
        }
    }

    private void runStorageContract(JSONObject report) throws Exception {
        Context ce = this;
        Context de = createDeviceProtectedStorageContext();
        java.io.File marker = getFileStreamPath(MARKER);
        boolean seed = !marker.exists();
        String nonce = seed ? UUID.randomUUID().toString() : readFile(ce, MARKER);
        report.put("phase", seed ? "seed" : "restart");
        report.put("nonce", nonce);

        JSONObject checks = new JSONObject();
        report.put("checks", checks);
        checks.put("default_is_credential_protected", !ce.isDeviceProtectedStorage());
        checks.put("created_context_is_device_protected", de.isDeviceProtectedStorage());
        checks.put("file_paths_differ", !ce.getFileStreamPath(FILE).getCanonicalFile().equals(
                de.getFileStreamPath(FILE).getCanonicalFile()));
        checks.put("database_paths_differ", !ce.getDatabasePath(DB).getCanonicalFile().equals(
                de.getDatabasePath(DB).getCanonicalFile()));

        if (seed) {
            writeScope(ce, nonce + ":ce");
            writeScope(de, nonce + ":de");
        }

        checks.put("ce_file", readFile(ce, FILE).equals(nonce + ":ce"));
        checks.put("de_file", readFile(de, FILE).equals(nonce + ":de"));
        checks.put("ce_preferences", preferenceValue(ce).equals(nonce + ":ce"));
        checks.put("de_preferences", preferenceValue(de).equals(nonce + ":de"));
        checks.put("ce_database", databaseValue(ce).equals(nonce + ":ce"));
        checks.put("de_database", databaseValue(de).equals(nonce + ":de"));

        if (seed) {
            writeFile(ce, MARKER, nonce);
        }
    }

    private static void writeScope(Context context, String value) throws Exception {
        writeFile(context, FILE, value);
        SharedPreferences preferences = context.getSharedPreferences(PREFS, MODE_PRIVATE);
        if (!preferences.edit().putString("value", value).commit()) {
            throw new java.io.IOException("Could not commit preferences");
        }
        try (SQLiteDatabase db = context.openOrCreateDatabase(DB, MODE_PRIVATE, null)) {
            db.execSQL("CREATE TABLE IF NOT EXISTS entries (value TEXT NOT NULL)");
            db.execSQL("DELETE FROM entries");
            db.execSQL("INSERT INTO entries VALUES (?)", new Object[] {value});
        }
    }

    private static void writeFile(Context context, String file, String value) throws Exception {
        try (java.io.OutputStream stream = context.openFileOutput(file, MODE_PRIVATE)) {
            stream.write(value.getBytes(StandardCharsets.UTF_8));
        }
    }

    private static String readFile(Context context, String file) throws Exception {
        try (java.io.InputStream stream = context.openFileInput(file)) {
            return new String(stream.readAllBytes(), StandardCharsets.UTF_8);
        }
    }

    private static String preferenceValue(Context context) {
        return context.getSharedPreferences(PREFS, MODE_PRIVATE).getString("value", "");
    }

    private static String databaseValue(Context context) {
        if (!context.getDatabasePath(DB).isFile()) {
            return "";
        }
        try (SQLiteDatabase db = SQLiteDatabase.openDatabase(
                context.getDatabasePath(DB).getPath(), null, SQLiteDatabase.OPEN_READONLY);
             Cursor cursor = db.rawQuery("SELECT value FROM entries", null)) {
            return cursor.moveToFirst() && cursor.getCount() == 1 ? cursor.getString(0) : "";
        }
    }
}
