/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.Intent;
import android.net.Uri;

import java.util.HashSet;
import java.util.Set;
import java.util.concurrent.Executor;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.BooleanSupplier;
import java.util.function.Consumer;

/** ACTION_DIAL keeps its review step even when Windows' tel handler calls immediately. */
final class DialRequests {
    private static final int MAX_PENDING = 16;

    interface Backend {
        boolean confirm(String packageName, String number, int ownerPid, BooleanSupplier active);
        boolean open(String uri);
    }

    private final Executor mExecutor;
    private final Backend mBackend;
    private final Set<String> mPending = new HashSet<>();

    DialRequests() {
        this(Executors.newSingleThreadExecutor(work -> {
            Thread thread = new Thread(work, "dial-request");
            thread.setDaemon(true);
            return thread;
        }), new Backend() {
            @Override public boolean confirm(String pkg, String number, int pid, BooleanSupplier active) {
                return WindowsHost.confirmDial(pkg, number, pid, active);
            }
            @Override public boolean open(String uri) { return WindowsHost.openUrl(uri); }
        });
    }

    DialRequests(Executor executor, Backend backend) {
        mExecutor = executor;
        mBackend = backend;
    }

    static boolean handles(Intent intent) {
        return intent != null && Intent.ACTION_DIAL.equals(intent.getAction())
                && "tel".equals(intent.getScheme()) && WindowsUriHandlers.windowsUri(intent) != null;
    }

    /** Make control/bidi characters visible without changing the URI sent to Windows. */
    static String displayNumber(String uri) {
        String number = Uri.decode(uri.substring("tel:".length()));
        StringBuilder shown = new StringBuilder();
        for (int i = 0; i < number.length(); i++) {
            char c = number.charAt(i);
            if (Character.isISOControl(c) || Character.getType(c) == Character.FORMAT) {
                shown.append(String.format(java.util.Locale.ROOT, "\\u%04X", (int) c));
            } else {
                shown.append(c);
            }
        }
        return shown.toString();
    }

    void request(Intent intent, String packageName, int ownerPid, BooleanSupplier lifetime,
            BooleanSupplier handlerAvailable, Runnable beforeWork, Consumer<Boolean> completion) {
        if (!handles(intent)) throw new IllegalArgumentException("A numbered ACTION_DIAL is required");
        // Capture before returning to the caller; the confirmed address is exactly what opens.
        final String uri = WindowsUriHandlers.windowsUri(intent);
        // ShellExecute consumes a NUL-terminated string. Reject a literal NUL instead
        // of confirming a longer address than Windows would actually receive.
        if (uri.indexOf('\0') >= 0) { completion.accept(false); return; }
        final String number = displayNumber(uri);
        boolean accepted;
        synchronized (mPending) {
            accepted = mPending.size() < MAX_PENDING && mPending.add(packageName);
        }
        if (!accepted) { completion.accept(false); return; }
        AtomicBoolean started = new AtomicBoolean();
        try {
            mExecutor.execute(() -> {
                started.set(true);
                boolean opened = false;
                try {
                    beforeWork.run();
                    BooleanSupplier active = () -> !Thread.currentThread().isInterrupted()
                            && lifetime.getAsBoolean() && handlerAvailable.getAsBoolean();
                    if (active.getAsBoolean()
                            && mBackend.confirm(packageName, number, ownerPid, active)
                            && active.getAsBoolean()) {
                        // A dialog can outlive its caller, package generation or association.
                        // Never use the handler answer from before the user reviewed the number.
                        opened = mBackend.open(uri);
                    }
                } catch (RuntimeException | LinkageError failure) {
                    SystemServer.log("dial request failed: " + failure.getClass().getSimpleName());
                } finally {
                    synchronized (mPending) { mPending.remove(packageName); }
                    completion.accept(opened);
                }
            });
        } catch (RuntimeException rejected) {
            if (started.get()) throw rejected;
            synchronized (mPending) { mPending.remove(packageName); }
            completion.accept(false);
        }
    }
}
