/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.peer;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;

/** Private provider backed by a real app-cache file, delegated only by exact URI grant. */
public final class CallerContractProvider extends ContentProvider {
    private File file;
    @Override public boolean onCreate() {
        file = new File(getContext().getCacheDir(), "component-caller-data");
        try (FileOutputStream output = new FileOutputStream(file)) {
            output.write("component-caller-real-provider".getBytes(StandardCharsets.UTF_8));
        } catch (java.io.IOException failure) { throw new IllegalStateException("Cannot create real provider data", failure); }
        return true;
    }
    @Override public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (!"/item".equals(uri.getPath()) || !"r".equals(mode)) {
            throw new FileNotFoundException("Only the fixture item is readable");
        }
        return ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY);
    }
    @Override public String getType(Uri uri) { return "text/plain"; }
    @Override public Cursor query(Uri uri, String[] projection, String selection, String[] args, String order) {
        throw new UnsupportedOperationException("This fixture exposes file descriptors only");
    }
    @Override public Uri insert(Uri uri, ContentValues values) { throw new UnsupportedOperationException(); }
    @Override public int delete(Uri uri, String selection, String[] args) { throw new UnsupportedOperationException(); }
    @Override public int update(Uri uri, ContentValues values, String selection, String[] args) {
        throw new UnsupportedOperationException();
    }
}
