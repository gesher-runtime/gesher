/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.os.ParcelFileDescriptor;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.EOFException;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.io.FileDescriptor;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Consumer;

/** Bounded AOSP FileBridge WRITE/FSYNC/CLOSE protocol; APK bytes never include command headers. */
final class InstallFileBridge extends Thread {
    interface Reserve { void bytes(long growth) throws IOException; }
    interface Transport {
        ParcelFileDescriptor[] create() throws IOException;
        void cancel(ParcelFileDescriptor server, FileDescriptor target) throws IOException;
    }
    private static final AtomicInteger ACTIVE = new AtomicInteger();
    private static final ScheduledThreadPoolExecutor CANCELLER = new ScheduledThreadPoolExecutor(1, r -> {
        Thread t = new Thread(r, "package-install-cancel"); t.setDaemon(true); return t;
    });
    static { CANCELLER.setRemoveOnCancelPolicy(true); }
    private final RandomAccessFile mFile;
    private final ParcelFileDescriptor mServer, mClient;
    private final long mLimit;
    private final Reserve mReserve;
    private final Consumer<Boolean> mCompletion;
    private final Transport mTransport;
    private final Object mFdLock = new Object();
    private ScheduledFuture<?> mCancellation;
    private boolean mClosed, mTargetClosed;
    private volatile boolean mStopped;

    InstallFileBridge(File file, long offset, long limit, Reserve reserve,
            Consumer<Boolean> completion) throws IOException {
        this(file, offset, limit, reserve, completion, InstallPipeTransport.TRANSPORT);
    }
    /** Explicit server-local transport injection for offline protocol tests only. */
    InstallFileBridge(File file, long offset, long limit, Reserve reserve,
            Consumer<Boolean> completion, Transport transport) throws IOException {
        super("package-install-writer");
        setDaemon(true);
        if (ACTIVE.incrementAndGet() > 16) {
            ACTIVE.decrementAndGet(); throw new IOException("Too many installation writers");
        }
        RandomAccessFile target = null;
        try {
            target = new RandomAccessFile(file, "rw"); target.seek(offset);
            ParcelFileDescriptor[] pair = transport.create();
            mServer = pair[0]; mClient = pair[1]; mFile = target;
        } catch (IOException | RuntimeException e) {
            if (target != null) target.close();
            ACTIVE.decrementAndGet(); throw e;
        }
        mLimit = limit; mReserve = reserve; mCompletion = completion; mTransport = transport;
    }
    ParcelFileDescriptor client() { return mClient; }

    @Override public void run() {
        boolean finished = false;
        try {
            DataInputStream input = new DataInputStream(new FileInputStream(mServer.getFileDescriptor()));
            DataOutputStream output = new DataOutputStream(new FileOutputStream(mServer.getFileDescriptor()));
            byte[] buffer = new byte[65536];
            while (!mStopped) {
                int command = input.readInt(), length = input.readInt();
                if (command == 1) {
                    if (length < 0 || length > mLimit - mFile.getFilePointer()) throw new IOException("APK size limit");
                    while (length > 0) {
                        int read = input.read(buffer, 0, Math.min(buffer.length, length));
                        if (read < 0) throw new EOFException();
                        mReserve.bytes(Math.max(0, mFile.getFilePointer() + read - mFile.length()));
                        mFile.write(buffer, 0, read); length -= read;
                    }
                } else if (command == 2 || command == 3) {
                    mFile.getFD().sync();
                    if (command == 3) {
                        synchronized (mFdLock) { mFile.close(); mTargetClosed = true; }
                        // Publish completion before acknowledging CLOSE. A caller may
                        // commit as soon as its FileBridgeOutputStream.close returns.
                        mCompletion.accept(true); finished = true;
                    }
                    // FileBridgeOutputStream expects its eight-byte ACK in one read.
                    // Match AOSP's single write; two writeInt calls can expose a
                    // four-byte partial ACK to the waiting client.
                    output.write(new byte[] {0, 0, 0, (byte)command, 0, 0, 0, 0}); output.flush();
                    if (command == 3) break;
                } else throw new IOException("Invalid FileBridge command");
            }
        } catch (IOException ignored) {
            // Do not log caller paths, APK contents, or app metadata.
        } finally {
            closeOwnedDescriptors();
            try { if (!finished) mCompletion.accept(false); }
            finally { ACTIVE.decrementAndGet(); }
        }
    }
    void forceClose() {
        mStopped = true;
        synchronized (mFdLock) {
            if (mClosed || mCancellation != null) return;
            // CRT _close waits on a blocked _read's descriptor lock. CancelIoEx
            // does not wait; repeat to cover cancellation just before read starts.
            // Only the worker closes descriptors. No caller joins it under a
            // session lock, and fdLock prevents cancellation of a reused FD.
            mCancellation = CANCELLER.scheduleWithFixedDelay(() -> {
                synchronized (mFdLock) {
                    if (mClosed) return;
                    try { mTransport.cancel(mServer, mTargetClosed ? null : mFile.getFD()); }
                    catch (IOException | RuntimeException ignored) { }
                }
            }, 0, 10, TimeUnit.MILLISECONDS);
        }
    }
    private void closeOwnedDescriptors() {
        synchronized (mFdLock) {
            mClosed = true;
            if (mCancellation != null) mCancellation.cancel(false);
            try { mServer.close(); } catch (IOException ignored) { }
            if (!mTargetClosed) {
                try { mFile.close(); } catch (IOException ignored) { }
                mTargetClosed = true;
            }
        }
    }
}
