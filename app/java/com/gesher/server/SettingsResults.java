/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.Activity;

import java.security.SecureRandom;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;

/**
 * Results of per-app Settings screens that an app opened with startActivityForResult and that
 * Gesher shows as the Control Center's app details ({@link SettingsIntents}).
 *
 * <p>On a phone the Settings screen answers when the user leaves it, after deciding; an app
 * re-checks then (its onActivityResult, and the onResume around it). Here the result waits
 * for the Control Center: each request carries a token in its request file
 * ({@link ControlCenter}), and the Control Center reports the token when the details it
 * showed for it close ({@code gesher-app settings-closed}). The result is computed at that
 * moment ({@link #resultCode}): RESULT_OK only where AOSP's screen answers it, Install
 * unknown apps (ExternalSourcesDetails) once the app may request installs; RESULT_CANCELED
 * otherwise, as the other details screens set no result.
 *
 * <p>Nothing waits forever: a request whose details never report (a Control Center that
 * crashed, or a request it never read) is answered after {@link #TIMEOUT_MS}, and at most
 * {@link #MAX_PENDING} wait (the oldest is answered first). Delivery goes through the
 * activity manager's own checks: an activity that finished, died or was replaced meanwhile
 * gets nothing.
 */
final class SettingsResults {
    static final long TIMEOUT_MS = 15 * 60 * 1000L;
    static final int MAX_PENDING = 32;

    /** The waiting request: the caller's activity and the screen it opened. */
    static final class Pending {
        final Object source;
        final Object process;
        final String resultWho;
        final int requestCode;
        final String action;
        final String packageName;
        ScheduledFuture<?> timeout;

        Pending(Object source, Object process, String resultWho, int requestCode, String action,
                String packageName) {
            this.source = source;
            this.process = process;
            this.resultWho = resultWho;
            this.requestCode = requestCode;
            this.action = action;
            this.packageName = packageName;
        }
    }

    /** Hands a result to the activity manager (which drops it for a gone activity). */
    interface Delivery {
        void deliver(Pending pending, int resultCode);
    }

    /** Whether the screen's app may request installs now (canRequestPackageInstalls). */
    interface InstallAccess {
        boolean canRequestInstalls(String packageName);
    }

    private final Map<String, Pending> mPending = new LinkedHashMap<>();
    private final Delivery mDelivery;
    private final InstallAccess mInstalls;
    private final ScheduledExecutorService mTimer;
    private final long mTimeoutMs;
    private final SecureRandom mRandom = new SecureRandom();

    SettingsResults(Delivery delivery, InstallAccess installs) {
        this(delivery, installs, newTimer(), TIMEOUT_MS);
    }

    SettingsResults(Delivery delivery, InstallAccess installs, ScheduledExecutorService timer,
            long timeoutMs) {
        mDelivery = delivery;
        mInstalls = installs;
        mTimer = timer;
        mTimeoutMs = timeoutMs;
    }

    private static ScheduledExecutorService newTimer() {
        ScheduledThreadPoolExecutor timer = new ScheduledThreadPoolExecutor(1, r -> {
            Thread t = new Thread(r, "settings-results");
            t.setDaemon(true);
            return t;
        });
        timer.setRemoveOnCancelPolicy(true);
        return timer;
    }

    /** A token as the request file and the Control Center carry it: 32 hex digits. */
    static boolean validToken(String token) {
        return token != null && token.matches("[0-9a-f]{32}");
    }

    /** The result the screen answers now. */
    int resultCode(Pending pending) {
        if ("android.settings.MANAGE_UNKNOWN_APP_SOURCES".equals(pending.action)
                && pending.packageName != null) {
            try {
                if (mInstalls.canRequestInstalls(pending.packageName)) return Activity.RESULT_OK;
            } catch (RuntimeException e) {
                SystemServer.log("settings result: " + e);
            }
        }
        return Activity.RESULT_CANCELED;
    }

    /** Registers a request before the Control Center can see it; returns its token. */
    String register(Pending pending) {
        byte[] bytes = new byte[16];
        mRandom.nextBytes(bytes);
        StringBuilder token = new StringBuilder(32);
        for (byte b : bytes) token.append(String.format(java.util.Locale.ROOT, "%02x", b & 255));
        String key = token.toString();
        List<Pending> evicted = new ArrayList<>();
        synchronized (mPending) {
            while (mPending.size() >= MAX_PENDING) {
                Iterator<Map.Entry<String, Pending>> oldest = mPending.entrySet().iterator();
                evicted.add(oldest.next().getValue());
                oldest.remove();
            }
            mPending.put(key, pending);
            pending.timeout = mTimer.schedule(() -> answer(key), mTimeoutMs, TimeUnit.MILLISECONDS);
        }
        for (Pending old : evicted) finish(old);
        return key;
    }

    /** The request never reached the Control Center (it did not open): no result here. */
    void discard(String token) {
        Pending pending;
        synchronized (mPending) {
            pending = mPending.remove(token);
        }
        if (pending != null && pending.timeout != null) pending.timeout.cancel(false);
    }

    /** The Control Center closed the details shown for these requests. */
    int closed(String[] tokens) {
        int answered = 0;
        if (tokens == null) return 0;
        for (String token : tokens) {
            if (validToken(token) && answer(token)) answered++;
        }
        return answered;
    }

    private boolean answer(String token) {
        Pending pending;
        synchronized (mPending) {
            pending = mPending.remove(token);
        }
        if (pending == null) return false;
        finish(pending);
        return true;
    }

    private void finish(Pending pending) {
        if (pending.timeout != null) pending.timeout.cancel(false);
        try {
            mDelivery.deliver(pending, resultCode(pending));
        } catch (RuntimeException e) {
            SystemServer.log("settings result: delivery failed: " + e);
        }
    }

    int pendingCount() {
        synchronized (mPending) {
            return mPending.size();
        }
    }
}
