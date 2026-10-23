/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.components;

import android.content.ContentProvider;
import android.content.ContentUris;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;

import java.util.ArrayList;
import java.util.List;

/** The app's own provider: items in memory, content://com.gesher.components.items/items. */
public class ItemsProvider extends ContentProvider {
    static final String AUTHORITY = "com.gesher.components.items";
    static final Uri ITEMS = Uri.parse("content://" + AUTHORITY + "/items");
    static final String NAME = "name";
    static volatile boolean sCreated;

    private final List<String> mItems = new ArrayList<>();

    @Override
    public boolean onCreate() {
        sCreated = true;
        Report.line("ItemsProvider onCreate");
        return true;
    }

    @Override
    public synchronized Cursor query(Uri uri, String[] projection, String selection,
            String[] selectionArgs, String sortOrder) {
        MatrixCursor c = new MatrixCursor(new String[] {"_id", NAME});
        for (int i = 0; i < mItems.size(); i++) {
            c.addRow(new Object[] {i + 1, mItems.get(i)});
        }
        return c;
    }

    @Override
    public String getType(Uri uri) {
        return "vnd.android.cursor.dir/vnd.gesher.item";
    }

    @Override
    public synchronized Uri insert(Uri uri, ContentValues values) {
        mItems.add(values.getAsString(NAME));
        return ContentUris.withAppendedId(ITEMS, mItems.size());
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        return 0;
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        return 0;
    }
}
