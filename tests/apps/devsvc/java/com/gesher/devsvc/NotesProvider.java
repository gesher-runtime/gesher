/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.devsvc;

import android.content.ContentProvider;
import android.content.ContentUris;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;

import java.util.concurrent.atomic.AtomicLong;

/** A provider whose inserts notify their URI (ContentActivity's job trigger watches them). */
public class NotesProvider extends ContentProvider {
    static final Uri ITEMS = Uri.parse("content://com.gesher.devsvc.notes/items");
    private final AtomicLong mNext = new AtomicLong(1);

    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        Uri item = ContentUris.withAppendedId(ITEMS, mNext.getAndIncrement());
        getContext().getContentResolver().notifyChange(item, null);
        return item;
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs,
            String sortOrder) {
        return new MatrixCursor(new String[] {"_id"});
    }

    @Override
    public String getType(Uri uri) {
        return "vnd.android.cursor.item/vnd.gesher.note";
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
