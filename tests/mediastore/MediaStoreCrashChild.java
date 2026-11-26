/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.content.ContentValues;
import android.database.sqlite.SQLiteDatabase;
import android.net.Uri;

import java.io.File;

/**
 * A system server that dies inside a MediaStore operation. It opens the index the parent
 * harness prepared, arms a TEMP trigger that halts the JVM when the operation reaches the
 * database statement following its file move or file deletion, and runs one production
 * operation. The process stops between the filesystem change and the SQLite commit, as a
 * power loss or a killed system server would; the parent then restarts the index.
 *
 * Arguments: mode (rename | publish | delete | expire), database file, volume directory,
 * journal directory, clock (ms), item URI (unused for expire).
 */
public final class MediaStoreCrashChild {
    static final int HALTED = 77;

    public static void main(String[] argv) {
        String mode = argv[0];
        File dbFile = new File(argv[1]);
        MediaStoreHost.Clock clock = new MediaStoreHost.Clock();
        clock.now = Long.parseLong(argv[4]);
        MediaStoreHost.TestHost host = new MediaStoreHost.TestHost(new File(argv[2]), clock, new File(argv[3]));
        SQLiteDatabase db = SQLiteDatabase.openOrCreateDatabase(dbFile, null);
        MediaStoreIndex index = new MediaStoreIndex(db, host);
        db.createTestFunction("gesher_test_halt", () -> Runtime.getRuntime().halt(HALTED));
        boolean update = mode.equals("rename") || mode.equals("publish");
        db.execSQL(update
                ? "CREATE TEMP TRIGGER crash BEFORE UPDATE ON files WHEN NEW._data IS NOT OLD._data "
                        + "BEGIN SELECT gesher_test_halt(); END"
                : "CREATE TEMP TRIGGER crash BEFORE DELETE ON files BEGIN SELECT gesher_test_halt(); END");
        MediaStoreHost.App camera = new MediaStoreHost.App("com.example.camera", 10101, 35);
        Uri item = argv.length > 5 ? Uri.parse(argv[5]) : null;
        ContentValues values = new ContentValues();
        switch (mode) {
            case "rename":
                values.put("_display_name", "crashed.png");
                index.update(camera, item, values, MediaStoreHost.args());
                break;
            case "publish":
                values.put("is_pending", 0);
                index.update(camera, item, values, MediaStoreHost.args());
                break;
            case "delete":
                index.delete(camera, item, MediaStoreHost.args());
                break;
            case "expire":
                index.deleteExpired();
                break;
            default:
                throw new IllegalArgumentException(mode);
        }
        // The trigger did not fire: the operation never reached the statement after its file change.
        System.exit(0);
    }
}
