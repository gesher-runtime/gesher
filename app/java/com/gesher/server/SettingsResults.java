/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.Activity;

import java.security.SecureRandom;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.function.Predicate;

/**
 * Completion of per-app Settings screens that an app opened and that Gesher shows as the
 * Control Center's app details ({@link SettingsIntents}).
 *
 * <p>On a phone the Settings screen answers when the user leaves it, after deciding; an app
 * re-checks then (its onActivityResult, and the onResume around it). Here the result waits
 * for the Control Center: each request carries a token in its request file
 * ({@link ControlCenter}), and the Control Center reports the token when the details it
 * showed for it close ({@code gesher-app settings-closed}). The result is what AOSP's screen
 * answers ({@link #resultCode}): Install unknown apps (ExternalSourcesDetails) sets a result
 * only when the user turns its switch during the visit, RESULT_OK if the last turn was on and
 * RESULT_CANCELED if off ({@link #toggled}); a visit without a turn, and the other details
 * screens, which set no result, answer RESULT_CANCELED. Each queued request keeps
 * its own token until the host claims and closes that visit ({@link ControlCenter}).
 *
 * <p>Unshown requests expire. Once the host acknowledges a visible dialog, the request
 * waits for an explicit close or the end of that exact host process. The bounded table
 * rejects new requests when full without dismissing a visible visit.
 */
final class SettingsResults {
    static final long TIMEOUT_MS = 15 * 60 * 1000L;
    static final int MAX_PENDING = 32;
    private static final long PANEL_CHECK_MS = 5_000;

    record Panel(int pid, long startedAt) {
        boolean alive() {
            return pid > 0 && startedAt != 0 && Zygote.alive(pid, startedAt);
        }
    }

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
        Panel panel;
        /** The last turn of the screen's switch during the visit (null: none). */
        Boolean toggle;
        Runnable releaseCover;
        java.util.function.Consumer<Boolean> continuation;
        boolean hostClosed;

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

    private final Map<String, Pending> mPending = new HashMap<>();
    private final Delivery mDelivery;
    private final ScheduledExecutorService mTimer;
    private final long mTimeoutMs, mPanelCheckMs;
    private final Predicate<Panel> mPanelAlive;
    private final SecureRandom mRandom = new SecureRandom();

    SettingsResults(Delivery delivery) {
        this(delivery, newTimer(), TIMEOUT_MS, PANEL_CHECK_MS, Panel::alive);
    }

    SettingsResults(Delivery delivery, ScheduledExecutorService timer, long timeoutMs,
            long panelCheckMs, Predicate<Panel> panelAlive) {
        mDelivery = delivery;
        mTimer = timer;
        mTimeoutMs = timeoutMs;
        mPanelCheckMs = panelCheckMs;
        mPanelAlive = panelAlive;
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

    /** Binds the requests that are now visible to this instance of the Control Center. */
    int shown(Panel panel, String[] tokens) {
        if (!mPanelAlive.test(panel)) return 0;
        int count = 0;
        synchronized (mPending) {
            for (String token : tokens) {
                Pending pending = mPending.get(token);
                if (pending != null && connectPanel(pending, panel)) count++;
            }
        }
        return count;
    }

    private boolean connectPanel(Pending pending, Panel panel) {
        if (pending.panel != null) return panel.equals(pending.panel);
        pending.panel = panel;
        pending.timeout.cancel(false);
        pending.timeout = mTimer.schedule(() -> checkPanel(pending), mPanelCheckMs,
                TimeUnit.MILLISECONDS);
        return true;
    }

    private void checkPanel(Pending pending) {
        synchronized (mPending) {
            if (mPending.get(pending.token) != pending) return;
            if (mPanelAlive.test(pending.panel)) {
                pending.timeout = mTimer.schedule(() -> checkPanel(pending), mPanelCheckMs,
                        TimeUnit.MILLISECONDS);
                return;
            }
            mPending.remove(pending.token);
        }
        finish(pending, Activity.RESULT_CANCELED);
    }

    /** Record the committed switch only for the visible visit(s) it occurred in. */
    void toggled(Panel panel, String[] tokens, String packageName, String opName, boolean allowed) {
        if (!"android:request_install_packages".equals(opName) || !mPanelAlive.test(panel)) return;
        synchronized (mPending) {
            for (String token : tokens) {
                Pending pending = mPending.get(token);
                if (pending != null && UNKNOWN_SOURCES.equals(pending.action)
                        && packageName.equals(pending.packageName) && connectPanel(pending, panel)) {
                    pending.toggle = allowed;
                }
            }
        }
    }

    /** Registers before the host can see the request, or returns null if the table is full. */
    String register(Pending pending) {
        byte[] bytes = new byte[16];
        mRandom.nextBytes(bytes);
        StringBuilder token = new StringBuilder(32);
        for (byte b : bytes) token.append(String.format(java.util.Locale.ROOT, "%02x", b & 255));
        String key = token.toString();
        synchronized (mPending) {
            if (mPending.size() >= MAX_PENDING) return null;
            pending.token = key;
            mPending.put(key, pending);
            pending.timeout = mTimer.schedule(() -> abandonUnshown(pending), mTimeoutMs,
                    TimeUnit.MILLISECONDS);
        }
        return key;
    }

    private void abandonUnshown(Pending pending) {
        synchronized (mPending) {
            if (mPending.get(pending.token) != pending || pending.panel != null) return;
            mPending.remove(pending.token);
        }
        finish(pending, Activity.RESULT_CANCELED);
    }

    /** The request never reached the Control Center (it did not open): no result here. */
    void discard(String token) {
        Pending pending;
        synchronized (mPending) {
            pending = mPending.remove(token);
        }
        if (pending != null) {
            if (pending.timeout != null) pending.timeout.cancel(false);
            if (pending.releaseCover != null) pending.releaseCover.run();
        }
    }

    /** Only the host that displayed a visible visit may report its close. */
    int closed(Panel panel, String[] tokens) {
        int answered = 0;
        if (tokens == null) return 0;
        for (String token : tokens) {
            if (!validToken(token)) continue;
            Pending pending;
            synchronized (mPending) {
                pending = mPending.get(token);
                if (pending == null || (pending.panel != null && !pending.panel.equals(panel))) continue;
                mPending.remove(token);
                pending.hostClosed = pending.panel != null && pending.panel.equals(panel);
            }
            finish(pending, resultCode(pending));
            answered++;
        }
        return answered;
    }

    /** Cancels an admitted request before the host displayed it. */
    void cancel(String token) {
        Pending pending;
        synchronized (mPending) {
            pending = mPending.get(token);
        }
        if (pending != null) abandonUnshown(pending);
    }

    private void finish(Pending pending, int resultCode) {
        if (pending.timeout != null) pending.timeout.cancel(false);
        try {
            mDelivery.deliver(pending, resultCode);
        } catch (RuntimeException e) {
            SystemServer.log("settings result: delivery failed: " + e);
        } finally {
            if (pending.releaseCover != null) pending.releaseCover.run();
        }
    }

    int pendingCount() {
        synchronized (mPending) {
            return mPending.size();
        }
    }
}
