/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.app.ContentProviderHolder;
import android.content.AttributionSource;
import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.ProviderInfo;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.IBinder;
import android.os.ParcelFileDescriptor;
import android.os.Process;
import android.provider.OpenableColumns;

import java.io.FileNotFoundException;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.HashMap;
import java.util.Map;
import java.util.UUID;

/** Read-only snapshots of native clipboard images, shared with authorized paste targets. */
final class ClipboardImageProvider extends ContentProvider {
    static final String AUTHORITY = "com.gesher.clipboard.images";
    private static final String MIME_TYPE = "image/bmp";
    private static final String DISPLAY_NAME = "clipboard.bmp";
    private final Map<Uri, byte[]> mImages = new HashMap<>();
    private final ProviderInfo mInfo;
    private final CallerIdentity mCallers;
    private final UriGrants mGrants;
    final ContentProviders.RegisteredProvider registration;

    ClipboardImageProvider(ContentProviders providers) {
        mCallers = providers.callers();
        mGrants = providers.uriGrants();
        ApplicationInfo app = new ApplicationInfo();
        app.packageName = "android";
        app.processName = "system";
        app.uid = Process.SYSTEM_UID;
        app.flags = ApplicationInfo.FLAG_SYSTEM | ApplicationInfo.FLAG_PERSISTENT;
        app.enabled = true;
        mInfo = new ProviderInfo();
        mInfo.applicationInfo = app;
        mInfo.packageName = app.packageName;
        mInfo.processName = app.processName;
        mInfo.name = getClass().getName();
        mInfo.authority = AUTHORITY;
        mInfo.enabled = true;
        mInfo.grantUriPermissions = true;
        registration = new ContentProviders.RegisteredProvider(mInfo, getIContentProvider());
        providers.registerProvider(registration);
    }

    synchronized ContentProviderHolder holder() {
        if (getContext() == null) attachTo(android.app.GesherSettingsContext.get());
        ContentProviderHolder result = new ContentProviderHolder(new ProviderInfo(mInfo));
        result.provider = getIContentProvider();
        result.noReleaseNeeded = true;
        return result;
    }

    synchronized void attachTo(Context base) {
        if (getContext() != null) return;
        attachInfo(new ContextWrapper(base) {
            @Override public int checkUriPermission(Uri uri, int pid, int uid, int modes) {
                return mGrants.checkFull(uri, pid, uid, modes, 0);
            }
        }, mInfo);
    }

    synchronized Uri add(byte[] image) {
        Uri uri = new Uri.Builder().scheme("content").authority(AUTHORITY)
                .appendPath(UUID.randomUUID().toString()).build();
        mImages.put(uri, image);
        return uri;
    }

    synchronized void remove(Uri uri) { mImages.remove(uri); }

    void grantRead(IBinder owner, String targetPackage, Uri uri) {
        mGrants.grantFromProvider(registration, owner, targetPackage, uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
    }

    private synchronized byte[] contents(Uri uri) { return mImages.get(uri); }

    @Override public boolean onCreate() { return true; }

    private void authenticate(AttributionSource source) {
        if (mCallers.isSystem()) return;
        if (source == null || source.getUid() != mCallers.callerUid()) {
            throw new SecurityException("Clipboard image attribution must identify its Binder caller");
        }
        mCallers.requirePackage(source.getPackageName());
    }

    @Override protected int enforceReadPermissionInner(Uri uri, AttributionSource source) {
        authenticate(source);
        return super.enforceReadPermissionInner(uri, source);
    }

    @Override protected int enforceWritePermissionInner(Uri uri, AttributionSource source) {
        authenticate(source);
        return super.enforceWritePermissionInner(uri, source);
    }

    @Override public String getType(Uri uri) { return contents(uri) != null ? MIME_TYPE : null; }

    @Override public String getTypeAnonymous(Uri uri) { return null; }

    @Override public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs,
            String sortOrder) {
        byte[] bytes = contents(uri);
        String[] columns = projection == null
                ? new String[]{OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE} : projection;
        MatrixCursor result = new MatrixCursor(columns, bytes == null ? 0 : 1);
        if (bytes != null) {
            Object[] row = new Object[columns.length];
            for (int i = 0; i < columns.length; i++) {
                if (OpenableColumns.DISPLAY_NAME.equals(columns[i])) row[i] = DISPLAY_NAME;
                if (OpenableColumns.SIZE.equals(columns[i])) row[i] = bytes.length;
            }
            result.addRow(row);
        }
        return result;
    }

    @Override public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (!"r".equals(mode)) throw new FileNotFoundException("Clipboard image is read-only");
        byte[] bytes = contents(uri);
        if (bytes == null) throw new FileNotFoundException("Clipboard image is no longer current");
        Path temporary = null;
        ParcelFileDescriptor fd = null;
        try {
            // A fresh open gives each paste operation an independent offset. Unlink
            // the backing before delivering its read-only descriptor to the caller.
            temporary = Files.createTempFile("clipboard-image-", ".bmp");
            Files.write(temporary, bytes);
            fd = ParcelFileDescriptor.open(temporary.toFile(), ParcelFileDescriptor.MODE_READ_ONLY);
            Files.delete(temporary);
            return fd;
        } catch (IOException failure) {
            if (fd != null) {
                try { fd.close(); } catch (IOException cleanup) { failure.addSuppressed(cleanup); }
            }
            if (temporary != null) {
                try { Files.deleteIfExists(temporary); } catch (IOException cleanup) { failure.addSuppressed(cleanup); }
            }
            FileNotFoundException error = new FileNotFoundException("Could not open a clipboard image snapshot");
            error.initCause(failure);
            throw error;
        }
    }

    @Override public Uri insert(Uri uri, ContentValues values) { throw new UnsupportedOperationException(); }
    @Override public int delete(Uri uri, String selection, String[] selectionArgs) { throw new UnsupportedOperationException(); }
    @Override public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException();
    }
}
