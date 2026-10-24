/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.peer;

import android.app.Application;
import android.content.ContentProvider;
import android.content.ContentUris;
import android.content.ContentValues;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.Process;

import java.util.ArrayList;
import java.util.List;

/**
 * Notes in memory, for other packages: content://com.gesher.peer.notes/notes. insert adds
 * one, getType names the type, call("describe") answers the notes and this process's pid.
 * (Queries return no cursor across processes yet: CursorWindow's natives are not
 * registered.)
 */
public class NotesProvider extends ContentProvider {
    static final String AUTHORITY = "com.gesher.peer.notes";
    static final Uri NOTES = Uri.parse("content://" + AUTHORITY + "/notes");

    private final List<String> mNotes = new ArrayList<>();

    @Override
    public boolean onCreate() {
        PeerActivity.report("NotesProvider onCreate in process "
                + Application.getProcessName() + " (pid " + Process.myPid() + ")");
        return true;
    }

    @Override
    public synchronized Uri insert(Uri uri, ContentValues values) {
        mNotes.add(values.getAsString("text"));
        PeerActivity.report("NotesProvider insert " + values.getAsString("text")
                + " from pid " + android.os.Binder.getCallingPid());
        return ContentUris.withAppendedId(NOTES, mNotes.size());
    }

    @Override
    public String getType(Uri uri) {
        return "vnd.android.cursor.dir/vnd.gesher.note";
    }

    @Override
    public synchronized Bundle call(String method, String arg, Bundle extras) {
        Bundle b = new Bundle();
        b.putInt("pid", Process.myPid());
        b.putString("notes", String.join(", ", mNotes));
        PeerActivity.report("NotesProvider call " + method);
        return b;
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection,
            String[] selectionArgs, String sortOrder) {
        return null;
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
