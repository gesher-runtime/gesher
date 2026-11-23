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
 * showed for it close ({@code gesher-app settings-closed}). The result is what AOSP's screen
 * answers ({@link #resultCode}): Install unknown apps (ExternalSourcesDetails) sets a result
 * only when the user turns its switch during the visit, RESULT_OK if the last turn was on and
 * RESULT_CANCELED if off ({@link #toggled}); a visit without a turn, and the other details
 * screens, which set no result, answer RESULT_CANCELED. A request that a later one for
 * another app replaced before it was shown is answered at once ({@link ControlCenter}).
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
        String token;
        ScheduledFuture<?> timeout;
        /** The last turn of the screen's switch during the visit (null: none). */
        Boolean toggle;

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

    private final Map<String, Pending> mPending = new LinkedHashMap<>();
    private final Delivery mDelivery;
    private final ScheduledExecutorService mTimer;
    private final long mTimeoutMs;
    private final SecureRandom mRandom = new SecureRandom();

    SettingsResults(Delivery delivery) {
        this(delivery, newTimer(), TIMEOUT_MS);
    }

    SettingsResults(Delivery delivery, ScheduledExecutorService timer, long timeoutMs) {
        mDelivery = delivery;
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

    static final String UNKNOWN_SOURCES = "android.settings.MANAGE_UNKNOWN_APP_SOURCES";

    /** The result the screen answers now. */
    int resultCode(Pending pending) {
        synchronized (mPending) {
            return UNKNOWN_SOURCES.equals(pending.action) && Boolean.TRUE.equals(pending.toggle)
                    ? Activity.RESULT_OK : Activity.RESULT_CANCELED;
        }
    }

    /**
     * The user turned a special access switch (the Control Center's details): the waiting
     * requests of that app's screen for it remember the last turn (ExternalSourcesDetails'
     * setResult on a change).
     */
    void toggled(String packageName, String opName, boolean allowed) {
        if (!"android:request_install_packages".equals(opName)) return;
        synchronized (mPending) {
            for (Pending pending : mPending.values()) {
                if (UNKNOWN_SOURCES.equals(pending.action) && pending.packageName != null
                        && pending.packageName.equals(packageName)) {
                    pending.toggle = allowed;
                }
            }
        }
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
            pending.token = key;
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
