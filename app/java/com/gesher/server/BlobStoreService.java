/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.blob.BlobHandle;
import android.app.blob.BlobInfo;
import android.app.blob.IBlobStoreManager;
import android.app.blob.IBlobStoreSession;
import android.app.blob.LeaseInfo;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.os.RemoteCallback;

import java.util.Collections;
import java.util.List;
import java.util.Objects;

/**
 * Explicit absence of an Android shared-blob storage backend. Apps can enumerate
 * their empty leases and discover zero quota; no session, descriptor or lease is
 * fabricated. Storage and mutation requests fail rather than reporting success.
 */
public final class BlobStoreService extends IBlobStoreManager.Default {
    private static SecurityException unavailable() {
        return new SecurityException("Android shared-blob storage is unavailable");
    }

    @Override public List<BlobHandle> getLeasedBlobs(String packageName) {
        return Collections.emptyList();
    }

    @Override public List<BlobInfo> queryBlobsForUser(int userId) {
        return Collections.emptyList();
    }

    @Override public long getRemainingLeaseQuotaBytes(String packageName) { return 0; }

    @Override public LeaseInfo getLeaseInfo(BlobHandle handle, String packageName) {
        // The public API requires SecurityException for an absent/inaccessible
        // blob; null would imply an accessible blob with no active caller lease.
        throw unavailable();
    }

    @Override public void waitForIdle(RemoteCallback callback) {
        // No storage tasks exist. Complete the framework's bounded idle wait.
        Objects.requireNonNull(callback, "callback").sendResult(new Bundle());
    }

    @Override public long createSession(BlobHandle handle, String packageName) {
        throw unavailable();
    }

    @Override public IBlobStoreSession openSession(long id, String packageName) {
        throw unavailable();
    }

    @Override public ParcelFileDescriptor openBlob(BlobHandle handle, String packageName) {
        throw unavailable();
    }

    @Override public void abandonSession(long id, String packageName) { }

    @Override public void acquireLease(BlobHandle handle, int descriptionResId,
            CharSequence description, long timeoutMillis, String packageName) {
        throw unavailable();
    }

    @Override public void releaseLease(BlobHandle handle, String packageName) { }
    @Override public void releaseAllLeases(String packageName) { }
    @Override public void deleteBlob(long id) { }
}
