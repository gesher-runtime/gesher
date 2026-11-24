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
 * <li>Windows answers {@code Available} now.
 * </ul>
 *
 * <p>{@link #supported} is {@code FEATURE_SECURE_LOCK_SCREEN}, which Android fixes at boot.
 * It is decided once, from Windows' answer when the system server starts: {@code Available}
 * or {@code NotConfiguredForUser} (Windows has a verifier; the user may or may not have set
 * it up, as a phone's user may or may not have set a screen lock). Any other answer declares
 * no secure lock screen for this run, and then no credential is offered either, as on an
 * Android device without the feature, where no credential can be set. Removing Windows Hello
 * later only makes the device not secure; setting it up later makes it secure, when it was
 * supported at start.
 */
final class DeviceCredential {
    /** Microsoft's minimum build for RequestVerificationForWindowAsync (Windows 11). */
    static final int MINIMUM_WINDOWS_BUILD = 22000;

    private static DeviceCredential sSystem;

    private final WindowsHelloBackend mBackend;  // null: no natives, or Windows is too old
    private final int mStartState;
    private final boolean mSupported;

    /**
     * @param backend Windows Hello, or null without its natives
     * @param windowsBuild Windows' build number (RtlGetVersion), 0 when unknown
     */
    DeviceCredential(WindowsHelloBackend backend, int windowsBuild) {
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
            mStartState = backend.availability();
        }
        mSupported = mBackend != null && (mStartState == WindowsHelloBackend.AVAILABLE
                || mStartState == WindowsHelloBackend.NOT_CONFIGURED_FOR_USER);
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
        return mBackend.availability();
    }

    /** The user has a credential that can be confirmed now. */
    boolean available() {
        return state() == WindowsHelloBackend.AVAILABLE;
    }

    /** What confirms the credential; null when none can be offered. */
    WindowsHelloBackend backend() {
        return mSupported ? mBackend : null;
    }
}
