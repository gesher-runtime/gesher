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
 * {@code window}: WindowManagerService, minimal. No windows are added while headless:
 * {@link #openSession} hands out a stand-in {@link IWindowSession} whose methods all
 * answer defaults, unless a session factory is set ({@link #setSessionFactory}): that is
 * where a real window session (the graphics side's) plugs in.
 */
public final class WindowManagerService extends IWindowManager.Default {
    private static volatile Function<IWindowSessionCallback, IWindowSession> sSessionFactory;

    private final StandIn<IWindowSession> mSession =
            StandIn.of(IWindowSession.class, new IWindowSession.Default());

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
