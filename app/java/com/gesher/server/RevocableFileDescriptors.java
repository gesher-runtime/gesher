/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.os.ParcelFileDescriptor;
import java.io.FileDescriptor;
import java.io.IOException;

/** One revocation barrier shared by a session's descriptors and their aliases. */
final class RevocableFileDescriptors implements AutoCloseable {
    private long mController;

    RevocableFileDescriptors() throws IOException {
        try {
            mController = nativeCreate();
        } catch (UnsatisfiedLinkError unavailable) {
            throw new IOException("Revocable file descriptors require the Windows runtime", unavailable);
        }
        if (mController == 0) throw new IOException("Could not create descriptor revocation group");
    }

    synchronized void attach(ParcelFileDescriptor descriptor) throws IOException {
        if (mController == 0) throw new IOException("Descriptor session is closed");
        nativeAttach(mController, descriptor.getFileDescriptor());
    }

    /** Waits for admitted I/O, then prevents all subsequent descriptor I/O. */
    @Override public synchronized void close() throws IOException {
        if (mController == 0) return;
        nativeRevoke(mController);
        nativeRelease(mController);
        mController = 0;
    }

    private static native long nativeCreate() throws IOException;
    private static native void nativeAttach(long controller, FileDescriptor descriptor) throws IOException;
    private static native void nativeRevoke(long controller) throws IOException;
    private static native void nativeRelease(long controller);
}
