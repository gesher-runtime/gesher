/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app;

import android.content.Context;
import android.hardware.display.DisplayManagerGlobal;
import android.hardware.display.IDisplayManager;
import android.os.IBinder;
import android.os.ServiceManager;

import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Proxy;

/**
 * What an app process needs, on the client side, to use the system server's services
 * over binder ({@code gesher-app --system-server}) where Gesher still lacks natives.
 */
final class RemoteServices {
    private RemoteServices() {
    }

    /**
     * DisplayManagerGlobal over the remote {@code display} service, except for
     * {@code getOverlaySupport}, which answers null here. Reading its reply, even a null
     * one, initializes {@code android.hardware.OverlayProperties}, whose static
     * initializer calls a native from libandroid_runtime's
     * android_hardware_OverlayProperties.cpp (libgui) that Gesher does not register yet.
     * Every other IDisplayManager call goes to the system server. Remove this once those
     * natives are there.
     */
    static void installDisplayManagerGlobal() throws ReflectiveOperationException {
        IBinder b = ServiceManager.getService(Context.DISPLAY_SERVICE);
        if (b == null) {
            throw new IllegalStateException("no display service in servicemanager");
        }
        IDisplayManager remote = IDisplayManager.Stub.asInterface(b);
        IDisplayManager dm = (IDisplayManager) Proxy.newProxyInstance(
                IDisplayManager.class.getClassLoader(), new Class<?>[] {IDisplayManager.class},
                (proxy, method, args) -> {
                    if (method.getName().equals("getOverlaySupport")
                            && method.getParameterCount() == 0) {
                        return null;
                    }
                    try {
                        return method.invoke(remote, args);
                    } catch (InvocationTargetException e) {
                        throw e.getCause();
                    }
                });
        DisplayManagerGlobal global = new DisplayManagerGlobal(dm);
        Field f = DisplayManagerGlobal.class.getDeclaredField("sInstance");
        f.setAccessible(true);
        synchronized (DisplayManagerGlobal.class) {
            f.set(null, global);
        }
    }
}
