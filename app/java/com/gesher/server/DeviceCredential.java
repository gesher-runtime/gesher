/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

/**
 * Whether the user has a device credential Gesher can confirm (ADR 0091, phase 2b): the one
 * decision behind {@code trust}'s {@code isDeviceSecure}, the window manager's
 * {@code isKeyguardSecure}, {@code auth}'s {@code canAuthenticate(DEVICE_CREDENTIAL)}, the
 * confirm-credential activity and {@code FEATURE_SECURE_LOCK_SCREEN}, so they cannot disagree.
 *
 * <p>The credential is Windows Hello's, confirmed through {@link WindowsHelloBackend}. It is
 * available when all of these hold:
 * <ul>
 * <li>the Windows Hello natives are in this runtime;
 * <li>Windows is build {@value #MINIMUM_WINDOWS_BUILD} or later, the minimum Microsoft
 *     documents for {@code IUserConsentVerifierInterop::RequestVerificationForWindowAsync};
 * <li>the device supports a secure lock screen ({@link #supported});
 * <li>Windows answers {@code Available} now, or answers that a prompt cannot start right now
 *     ({@code DeviceBusy}, or a query that failed) after having answered {@code Available}
 *     within the last {@value #BUSY_TRUST_MS} ms.
 * </ul>
 *
 * <p>The last clause keeps two questions apart that Android keeps apart: whether the user
 * <em>has</em> a credential ({@link #available}: {@code isDeviceSecure}, {@code isKeyguardSecure},
 * {@code canAuthenticate}, FEATURE_SECURE_LOCK_SCREEN) and whether a prompt can start <em>now</em>
 * ({@link #state}: a prompt that cannot start ends with {@code BIOMETRIC_ERROR_HW_UNAVAILABLE},
 * never {@code NO_DEVICE_CREDENTIAL}). {@code DeviceBusy} says only that the device is occupied
 * (Microsoft: "The authentication device is performing an operation"); it says nothing about
 * the user's credential, so it neither creates one nor removes one. Only a definite answer
 * ({@code Available}, {@code NotConfiguredForUser}, {@code DisabledByPolicy},
 * {@code DeviceNotPresent}) changes what is remembered, and it does so at once.
 * The remembered answer is trusted for {@value #BUSY_TRUST_MS} ms: a busy device is
 * normally occupied for seconds, while one that stays unresponsive for minutes is a failing
 * Windows Hello, about which Gesher can no longer state that a credential exists.
 *
 * <p>{@link #supported} is {@code FEATURE_SECURE_LOCK_SCREEN}, which Android fixes at boot.
 * It is decided once, from Windows' answer when the system server starts: {@code Available}
 * or {@code NotConfiguredForUser} establishes that Windows has a verifier. A
 * {@code DeviceBusy} answer also establishes the verifier's presence, with enrollment unknown.
 * After fresh retries, a definite answer takes precedence over a previous busy answer. Other
 * answers that provide no evidence of a verifier leave the feature off for this run. A supported
 * device can become secure later when Windows confirms that its verifier is available.
 */
final class DeviceCredential {
    /** Microsoft's minimum build for RequestVerificationForWindowAsync (Windows 11). */
    static final int MINIMUM_WINDOWS_BUILD = 22000;
    /** How long a transient answer keeps the last definite one (decision 7 of ADR 0091). */
    static final long BUSY_TRUST_MS = 10 * 60 * 1000;
    /** Re-asks at start when Windows answers busy or fails, before FEATURE_SECURE_LOCK_SCREEN. */
    private static final int START_RETRIES = 3;
    private static final long START_RETRY_MS = 250;

    private static DeviceCredential sSystem;

    private final WindowsHelloBackend mBackend;  // null: no natives, or Windows is too old
    private final int mStartState;
    private final boolean mSupported;
    private final java.util.function.LongSupplier mClockMs;

    // The last definite answer (not DeviceBusy, not a failed query), and when it was given.
    private final Object mLock = new Object();
    private boolean mKnown;
    private boolean mKnownEnrolled;
    private long mKnownAtMs;

    /**
     * @param backend Windows Hello, or null without its natives
     * @param windowsBuild Windows' build number (RtlGetVersion), 0 when unknown
     */
    DeviceCredential(WindowsHelloBackend backend, int windowsBuild) {
        this(backend, windowsBuild, () -> System.nanoTime() / 1_000_000, millis -> {
            try {
                Thread.sleep(millis);
            } catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
            }
        });
    }

    /** As above, with the clock and the wait between start-up queries (milliseconds) given. */
    DeviceCredential(WindowsHelloBackend backend, int windowsBuild,
            java.util.function.LongSupplier clockMs, java.util.function.LongConsumer sleepMs) {
        mClockMs = clockMs;
        boolean supported = false;
        if (backend == null) {
            mBackend = null;
            mStartState = WindowsHelloBackend.FAILED;
        } else if (windowsBuild < MINIMUM_WINDOWS_BUILD) {
            mBackend = null;
            mStartState = WindowsHelloBackend.DEVICE_NOT_PRESENT;
            SystemServer.log("auth: Windows build " + windowsBuild + " is below "
                    + MINIMUM_WINDOWS_BUILD + ": no device credential");
        } else {
            mBackend = backend;
            int answer = backend.availability();
            boolean sawVerifier = answer == WindowsHelloBackend.DEVICE_BUSY;
            // A retry needs a new Windows sample. The normal availability cache outlasts
            // the entire startup retry window.
            for (int retry = 0; isTransient(answer) && retry < START_RETRIES; retry++) {
                sleepMs.accept(START_RETRY_MS);
                answer = backend.refreshAvailability();
                sawVerifier |= answer == WindowsHelloBackend.DEVICE_BUSY;
            }
            mStartState = answer;
            remember(answer);
            supported = answer == WindowsHelloBackend.AVAILABLE
                    || answer == WindowsHelloBackend.NOT_CONFIGURED_FOR_USER
                    || (isTransient(answer) && sawVerifier);
        }
        mSupported = supported;
        if (mBackend != null && !mSupported) {
            SystemServer.log("auth: Windows Hello answered " + mStartState
                    + " at start: no secure lock screen for this run");
        }
    }

    /** The system server's instance, over the real Windows Hello. */
    static synchronized DeviceCredential system() {
        if (sSystem == null) {
            WindowsHello hello = WindowsHello.create();
            sSystem = new DeviceCredential(hello, hello == null ? 0 : WindowsHello.windowsBuild());
        }
        return sSystem;
    }

    /** FEATURE_SECURE_LOCK_SCREEN: fixed for this run of the system server. */
    boolean supported() {
        return mSupported;
    }

    /**
     * Windows' answer as a {@link WindowsHelloBackend} availability value: the current one
     * when a credential can be offered at all, otherwise why it cannot (never AVAILABLE).
     */
    int state() {
        if (!mSupported) return mStartState;
        int answer = mBackend.availability();
        remember(answer);
        return answer;
    }

    /**
     * The user has a credential. It does not say that a prompt can start now: a busy Windows
     * Hello leaves this unchanged (for {@value #BUSY_TRUST_MS} ms after a definite answer),
     * and {@link #state} then says why no prompt can start.
     */
    boolean available() {
        if (!mSupported) return false;
        int answer = state();
        if (answer == WindowsHelloBackend.AVAILABLE) return true;
        if (!isTransient(answer)) return false;
        synchronized (mLock) {
            return mKnownEnrolled && hasRecentAnswer();
        }
    }

    /** A supported verifier did not answer whether the user is enrolled. */
    boolean enrollmentUnknown() {
        if (!mSupported || !isTransient(state())) return false;
        synchronized (mLock) {
            return !hasRecentAnswer();
        }
    }

    /** Called under mLock. */
    private boolean hasRecentAnswer() {
        return mKnown && mClockMs.getAsLong() - mKnownAtMs <= BUSY_TRUST_MS;
    }

    /** DeviceBusy or a failed query: nothing is known about the credential right now. */
    private static boolean isTransient(int answer) {
        return answer == WindowsHelloBackend.DEVICE_BUSY || answer == WindowsHelloBackend.FAILED;
    }

    private void remember(int answer) {
        if (isTransient(answer)) return;
        synchronized (mLock) {
            mKnown = true;
            mKnownEnrolled = answer == WindowsHelloBackend.AVAILABLE;
            mKnownAtMs = mClockMs.getAsLong();
        }
    }

    /** What confirms the credential; null when none can be offered. */
    WindowsHelloBackend backend() {
        return mSupported ? mBackend : null;
    }
}
