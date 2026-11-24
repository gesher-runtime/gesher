/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.graphics.Point;
import android.view.IWindowManager;
import android.view.IWindowSession;
import android.view.IWindowSessionCallback;

import com.gesher.app.DeviceProfile;

import java.util.function.Function;

/**
 * {@code window}: WindowManagerService, minimal. {@link #openSession} hands out
 * {@link StandInWindowSession} (windows are added headless: ADD_OKAY, the display's
 * frame, no surface), unless a session factory is set ({@link #setSessionFactory}): that
 * is where a real window session (the graphics side's) plugs in.
 */
public final class WindowManagerService extends IWindowManager.Default {
    private static volatile Function<IWindowSessionCallback, IWindowSession> sSessionFactory;
    private static final java.util.Map<String, String> sWindowModes =
            new java.util.concurrent.ConcurrentHashMap<>();

    /**
     * The window mode of the package's processes started from now on (what the launcher
     * asked: {@code window}, {@code hidden} or {@code headless};
     * com.gesher.app.window.WindowMode); null forgets it. App processes draw their own
     * windows (ADR 0029): the mode reaches them as {@code $GESHER_APP_WINDOW}.
     */
    public static void setWindowMode(String packageName, String mode) {
        if (packageName == null) return;
        String m = com.gesher.app.window.WindowMode.parse(mode);
        if (m == null) {
            sWindowModes.remove(packageName);
        } else {
            sWindowModes.put(packageName, m);
        }
    }

    /** The package's window mode ({@link #setWindowMode}), or null. */
    public static String windowMode(String packageName) {
        return packageName != null ? sWindowModes.get(packageName) : null;
    }

    /** Resolve one fresh process without converting inherited mode into host policy. */
    static String resolveWindowMode(String packageName, String sourceMode) {
        return resolveWindowPolicy(packageName, sourceMode).mode;
    }

    static final class WindowPolicy {
        final String mode;
        final boolean explicit;
        WindowPolicy(String mode, boolean explicit) { this.mode = mode; this.explicit = explicit; }
    }

    static WindowPolicy resolveWindowPolicy(String packageName, String sourceMode) {
        String explicit = windowMode(packageName);
        if (explicit != null) return new WindowPolicy(explicit, true);
        String inherited = com.gesher.app.window.WindowMode.parse(sourceMode);
        return new WindowPolicy(inherited != null ? inherited
                : com.gesher.app.window.WindowMode.fromEnvironment(
                        com.gesher.app.window.WindowMode.WINDOW), false);
    }

    private final StandIn<IWindowSession> mSession =
            StandIn.of(IWindowSession.class, new StandInWindowSession());

    /**
     * Makes {@link #openSession} answer {@code factory.apply(callback)}. The session it
     * returns is passed to other processes by its {@code asBinder()}, so a session meant
     * to serve them must be an {@code IWindowSession.Stub} (in-process, any implementation
     * does).
     */
    public static void setSessionFactory(
            Function<IWindowSessionCallback, IWindowSession> factory) {
        sSessionFactory = factory;
    }

    @Override
    public IWindowSession openSession(IWindowSessionCallback callback) {
        Function<IWindowSessionCallback, IWindowSession> factory = sSessionFactory;
        return factory != null ? factory.apply(callback) : mSession.asInterface();
    }

    /**
     * KeyguardManager.isKeyguardSecure: PhoneWindowManager answers whether the user has a
     * secure lock screen (or a locked SIM, which Gesher has not). The same decision as
     * {@code trust}'s isDeviceSecure ({@link DeviceCredential}, ADR 0094). As AOSP's, another
     * user's state needs INTERACT_ACROSS_USERS, which only the system holds here.
     */
    @Override
    public boolean isKeyguardSecure(int userId) {
        if (userId != android.os.UserHandle.getCallingUserId()
                && android.os.Binder.getCallingPid() != android.os.Process.myPid()
                && android.os.Binder.getCallingUid() != android.os.Process.SYSTEM_UID) {
            throw new SecurityException("Requires INTERACT_ACROSS_USERS permission");
        }
        return userId == android.os.UserHandle.USER_SYSTEM && DeviceCredential.system().available();
    }

    @Override
    public void getInitialDisplaySize(int displayId, Point size) {
        size.set(DeviceProfile.WIDTH, DeviceProfile.HEIGHT);
    }

    @Override
    public void getBaseDisplaySize(int displayId, Point size) {
        size.set(DeviceProfile.WIDTH, DeviceProfile.HEIGHT);
    }

    @Override
    public boolean hasNavigationBar(int displayId) {
        return false;
    }

    @Override
    public float getCurrentAnimatorScale() {
        return 1f;
    }

    @Override
    public float[] getAnimationScales() {
        return new float[] {1f, 1f, 1f};
    }
}
