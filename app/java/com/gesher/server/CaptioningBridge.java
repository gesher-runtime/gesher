/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import java.util.Map;
import java.util.Objects;

/**
 * Keeps the Settings.Secure caption keys that CaptioningManager reads equal to Windows'
 * caption style (CaptioningSettings), at start and whenever Windows reports a change.
 * A changed key notifies its observers through the settings provider, which is how
 * CaptioningManager's listeners (onUserStyleChanged, onFontScaleChanged) learn of it.
 * A failed or unknown Windows answer changes nothing.
 */
final class CaptioningBridge {
    /** What the bridge needs from Windows. */
    interface Backend {
        /** ClosedCaptionProperties as CaptioningSettings' values, or null when the read failed. */
        int[] read();

        /** Calls {@code onChange} (any thread) when the caption style may have changed. */
        Runnable watch(Runnable onChange);
    }

    /** The Secure table: a value, or null to remove the key. */
    interface Secure {
        String get(String key);

        void set(String key, String value);
    }

    private final Backend mWindows;
    private final Secure mSecure;
    private Runnable mUnwatch;  // guarded by this

    CaptioningBridge(Backend windows, Secure secure) {
        mWindows = windows;
        mSecure = secure;
    }

    synchronized void start() {
        apply();
        if (mUnwatch == null) mUnwatch = mWindows.watch(this::apply);
    }

    synchronized void stop() {
        if (mUnwatch != null) {
            mUnwatch.run();
            mUnwatch = null;
        }
    }

    /** Applies Windows' current style; false when Windows gave no usable answer. */
    synchronized boolean apply() {
        int[] windows;
        try {
            windows = mWindows.read();
        } catch (RuntimeException failed) {
            return false;
        }
        Map<String, String> values = CaptioningSettings.secureValues(windows);
        if (values == null) return false;
        for (Map.Entry<String, String> e : values.entrySet()) {
            if (!Objects.equals(mSecure.get(e.getKey()), e.getValue())) {
                mSecure.set(e.getKey(), e.getValue());
            }
        }
        return true;
    }
}
