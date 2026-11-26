/* SPDX-License-Identifier: Apache-2.0 */
package org.gesher.contracts.storagemigration;

import android.app.Activity;
import android.content.Context;
import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;
import android.os.Bundle;
import android.os.Process;
import android.os.UserManager;
import android.util.Log;

import java.nio.charset.StandardCharsets;
import java.util.UUID;

public final class MigrationActivity extends Activity {
    private static final String TAG = "StorageMigration";
    private static final String STATE = "fixture-state";
    private static final String PREFS = "migration";
    private static final String DB = "migration.db";
    private static final String KEY = "value";

    @Override protected void onCreate(Bundle savedState) {
        super.onCreate(savedState);
        try {
            runMigration();
        } catch (Exception | AssertionError failure) {
            Log.e(TAG, "FAIL pid=" + Process.myPid(), failure);
        } finally {
            finish();
        }
    }

    private void runMigration() throws Exception {
        require(getSystemService(UserManager.class).isUserUnlocked(), "unlocked user");
        Context ce = this;
        Context de = createDeviceProtectedStorageContext();
        require(!ce.isDeviceProtectedStorage() && de.isDeviceProtectedStorage(), "storage modes");
        require(!ce.getDataDir().getCanonicalFile().equals(de.getDataDir().getCanonicalFile()),
                "different data directories");

        String step;
        String token;
        if (!ce.getFileStreamPath(STATE).exists()) {
            token = UUID.randomUUID().toString();
            writeArea(ce, token);
            writeArea(de, "old-device");
            migrate(ce, de, token);
            writeState("device " + token);
            step = "ce-to-de";
        } else {
            String state;
            try (var stream = ce.openFileInput(STATE)) {
                state = new String(stream.readAllBytes(), StandardCharsets.UTF_8);
            }
            String[] fields = state.split(" ", 2);
            require(fields.length == 2, "state marker");
            token = fields[1];
            if ("device".equals(fields[0])) {
                verifyTransfer(ce, de, token);
                writeArea(ce, "old-credential");
                migrate(de, ce, token);
                writeState("credential " + token);
                step = "de-to-ce";
            } else {
                require("credential".equals(fields[0]), "phase marker");
                verifyTransfer(de, ce, token);
                step = "verified";
            }
        }
        Log.i(TAG, "PASS step=" + step + " pid=" + Process.myPid() + " fixture-id=" + token);
    }

    private void writeState(String state) throws Exception {
        try (var stream = openFileOutput(STATE, MODE_PRIVATE)) {
            stream.write(state.getBytes(StandardCharsets.UTF_8));
        }
    }

    private static void writeArea(Context context, String value) {
        require(context.getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                .putString(KEY, value).commit(), "commit preferences");
        try (SQLiteDatabase db = context.openOrCreateDatabase(DB, MODE_PRIVATE, null)) {
            db.execSQL("CREATE TABLE IF NOT EXISTS records (value TEXT NOT NULL)");
            db.execSQL("DELETE FROM records");
            db.execSQL("INSERT INTO records VALUES (?)", new Object[] {value});
        }
    }

    private static void migrate(Context source, Context target, String token) {
        // Both caches were populated, and the target already held its own different value.
        require(target.moveSharedPreferencesFrom(source, PREFS), "move preferences");
        require(target.moveDatabaseFrom(source, DB), "move database");
        verifyTransfer(source, target, token);
    }

    private static void verifyTransfer(Context source, Context target, String token) {
        require(!source.getSharedPreferences(PREFS, MODE_PRIVATE).contains(KEY),
                "source preference absent");
        require(!source.getDatabasePath(DB).exists(), "source database absent");
        require(token.equals(target.getSharedPreferences(PREFS, MODE_PRIVATE).getString(KEY, "")),
                "target preference value");
        try (SQLiteDatabase db = SQLiteDatabase.openDatabase(
                    target.getDatabasePath(DB).getPath(), null, SQLiteDatabase.OPEN_READONLY);
                Cursor cursor = db.rawQuery("SELECT value FROM records", null)) {
            require(cursor.moveToFirst() && cursor.getCount() == 1
                    && token.equals(cursor.getString(0)), "target database value");
        }
    }

    private static void require(boolean condition, String operation) {
        if (!condition) throw new AssertionError(operation);
    }
}
