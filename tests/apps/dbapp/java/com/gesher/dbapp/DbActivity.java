/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.dbapp;

import android.app.Activity;
import android.content.ContentValues;
import android.content.Context;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.database.DatabaseUtils;
import android.database.sqlite.SQLiteDatabase;
import android.database.sqlite.SQLiteOpenHelper;
import android.os.Bundle;

/**
 * Storage the way apps use it: a database file through SQLiteOpenHelper (onCreate, then
 * onUpgrade on a second open), inserts in a transaction, a query read through a Cursor
 * (CursorWindow), an ICU collation (the UNICODE collator libsqlite3_android registers),
 * an in-memory database, and SharedPreferences committed and read back.
 */
public class DbActivity extends Activity {
    static final class Helper extends SQLiteOpenHelper {
        final StringBuilder log = new StringBuilder();

        Helper(Context context, int version) {
            super(context, "notes.db", null, version);
        }

        @Override
        public void onCreate(SQLiteDatabase db) {
            db.execSQL("CREATE TABLE notes (_id INTEGER PRIMARY KEY, title TEXT, weight INTEGER)");
            log.append("onCreate ");
        }

        @Override
        public void onUpgrade(SQLiteDatabase db, int oldVersion, int newVersion) {
            db.execSQL("ALTER TABLE notes ADD COLUMN done INTEGER DEFAULT 0");
            log.append("onUpgrade ").append(oldVersion).append("->").append(newVersion).append(' ');
        }
    }

    /** {@code s} with non-ASCII characters as U+XXXX: the console's code page may not be UTF-8. */
    static String ascii(String s) {
        StringBuilder b = new StringBuilder();
        for (char ch : s.toCharArray()) {
            if (ch < 128) b.append(ch); else b.append(String.format("U+%04X", (int) ch));
        }
        return b.toString();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        try {
            run();
        } catch (Throwable t) {
            System.out.println("DbActivity failed: " + t);
            t.printStackTrace(System.out);
            throw t;
        }
    }

    private void run() {
        deleteDatabase("notes.db");
        Helper helper = new Helper(this, 1);
        SQLiteDatabase db = helper.getWritableDatabase();
        db.beginTransaction();
        try {
            String[] titles = {"zebra", "Apple", "éclair", "mango"};
            for (int i = 0; i < titles.length; i++) {
                ContentValues v = new ContentValues();
                v.put("title", titles[i]);
                v.put("weight", (i + 1) * 10);
                db.insertOrThrow("notes", null, v);
            }
            db.setTransactionSuccessful();
        } finally {
            db.endTransaction();
        }
        StringBuilder order = new StringBuilder();
        long sum = 0;
        try (Cursor c = db.query("notes", new String[] {"title", "weight"}, null, null, null, null,
                "title COLLATE UNICODE")) {
            while (c.moveToNext()) {
                if (order.length() > 0) order.append(',');
                order.append(ascii(c.getString(0)));
                sum += c.getLong(1);
            }
        }
        long rows = DatabaseUtils.queryNumEntries(db, "notes");
        String version = DatabaseUtils.stringForQuery(db, "SELECT sqlite_version()", null);
        helper.close();

        Helper upgraded = new Helper(this, 2);
        SQLiteDatabase db2 = upgraded.getReadableDatabase();
        long open = DatabaseUtils.longForQuery(db2, "SELECT COUNT(*) FROM notes WHERE done = 0", null);
        upgraded.close();
        String path = getDatabasePath("notes.db").getPath();
        boolean onDisk = getDatabasePath("notes.db").length() > 0;

        SQLiteDatabase mem = SQLiteDatabase.create(null);
        mem.execSQL("CREATE TABLE t (x)");
        mem.execSQL("INSERT INTO t VALUES (6), (7)");
        long product = DatabaseUtils.longForQuery(mem, "SELECT 6 * 7", null);
        mem.close();

        SharedPreferences prefs = getSharedPreferences("settings", MODE_PRIVATE);
        boolean committed = prefs.edit().putString("name", "Gesher").putInt("launches", 3).commit();
        SharedPreferences again = getSharedPreferences("settings", MODE_PRIVATE);

        System.out.println("DbActivity sqlite " + version + ": " + helper.log + upgraded.log.toString().trim()
                + ", " + rows + " rows, order " + order + ", sum " + sum + ", not done " + open
                + ", on disk " + onDisk + " (" + path + ")");
        System.out.println("DbActivity in-memory: " + product);
        System.out.println("DbActivity prefs: committed " + committed + ", name "
                + again.getString("name", null) + ", launches " + again.getInt("launches", 0));

        // Number and date formats, as list items format their dates (NoNonsense Notes'
        // DateView: SimpleDateFormat, ICU's NumberFormat service): each step on its own.
        icu("java.text.NumberFormat", () -> java.text.NumberFormat.getIntegerInstance().format(42));
        icu("icu NumberFormat", () -> android.icu.text.NumberFormat.getInstance(
                android.icu.util.ULocale.getDefault(), android.icu.text.NumberFormat.NUMBERSTYLE)
                .format(1234.5));
        icu("SimpleDateFormat", () -> new java.text.SimpleDateFormat("yyyy-MM-dd",
                java.util.Locale.US).format(new java.util.Date(0)));
        icu("DateFormat.getDateFormat", () -> android.text.format.DateFormat.getDateFormat(this)
                .format(new java.util.Date(0)));
        icu("icu DateFormat", () -> android.icu.text.DateFormat.getDateInstance(
                android.icu.text.DateFormat.MEDIUM).format(new java.util.Date(0)));
    }

    private static void icu(String step, java.util.concurrent.Callable<String> run) {
        try {
            System.out.println("DbActivity format " + step + ": ok " + ascii(run.call()));
        } catch (Throwable t) {
            System.out.println("DbActivity format " + step + ": failed " + t);
            t.printStackTrace(System.out);
        }
    }
}
