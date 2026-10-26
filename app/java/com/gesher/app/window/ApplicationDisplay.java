/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app.window;

import android.hardware.display.DisplayManagerGlobal;
import android.hardware.display.IDisplayManagerCallback;
import android.os.RemoteCallbackList;
import android.os.RemoteException;
import android.view.Display;
import android.view.DisplayInfo;
import android.view.Surface;

import com.gesher.app.DeviceProfile;

/**
 * The shared default display seen from an Application's DisplayManager. Activities
 * keep their own task configuration, including an explicit rotation of zero.
 * The shared display follows the focused visible task; resizing a task does not
 * change the display's natural dimensions. ADR 0061.
 *
 * <p>Lives in the system server for device mode and in the app for single-process
 * mode. Display listeners are real Binder callbacks, including mask updates and
 * removal when a client dies.
 */
public final class ApplicationDisplay {
    private static final RemoteCallbackList<IDisplayManagerCallback> CALLBACKS =
            new RemoteCallbackList<>();
    private static volatile int sRotation = Surface.ROTATION_0;
    private static long sSequence;

    private ApplicationDisplay() {
    }

    public static DisplayInfo info() {
        DisplayInfo info = DeviceProfile.displayInfo();
        int rotation = sRotation;
        if (rotation == Surface.ROTATION_90 || rotation == Surface.ROTATION_270) {
            info.appWidth = info.logicalWidth = DeviceProfile.HEIGHT;
            info.appHeight = info.logicalHeight = DeviceProfile.WIDTH;
        }
        info.rotation = rotation;
        int small = Math.min(DeviceProfile.WIDTH, DeviceProfile.HEIGHT);
        int large = Math.max(DeviceProfile.WIDTH, DeviceProfile.HEIGHT);
        info.smallestNominalAppWidth = info.smallestNominalAppHeight = small;
        info.largestNominalAppWidth = info.largestNominalAppHeight = large;
        return info;
    }

    public static void register(IDisplayManagerCallback callback, long mask) {
        if (callback == null) return;
        synchronized (CALLBACKS) {
            CALLBACKS.unregister(callback);
            CALLBACKS.register(callback, Long.valueOf(mask));
        }
    }

    /** Called outside the activity manager's lock, before focus notifications. */
    public static void update(int rotation) {
        synchronized (CALLBACKS) {
            update(rotation, sSequence + 1);
        }
    }

    /** An older focus calculation must not overwrite a newer concurrent one. */
    public static void update(int rotation, long sequence) {
        if (rotation < Surface.ROTATION_0 || rotation > Surface.ROTATION_270) {
            throw new IllegalArgumentException("display rotation " + rotation);
        }
        synchronized (CALLBACKS) {
            if (sequence <= sSequence) return;
            sSequence = sequence;
            if (rotation == sRotation) return;
            sRotation = rotation;
            DisplayManagerGlobal.invalidateLocalDisplayInfoCaches();
            int count = CALLBACKS.beginBroadcast();
            try {
                for (int i = 0; i < count; i++) {
                    long mask = ((Long) CALLBACKS.getBroadcastCookie(i)).longValue();
                    if ((mask & DisplayManagerGlobal.INTERNAL_EVENT_FLAG_DISPLAY_BASIC_CHANGED) == 0) {
                        continue;
                    }
                    try {
                        CALLBACKS.getBroadcastItem(i).onDisplayEvent(Display.DEFAULT_DISPLAY,
                                DisplayManagerGlobal.EVENT_DISPLAY_BASIC_CHANGED);
                    } catch (RemoteException ignored) {
                        // RemoteCallbackList removes the dead process.
                    }
                }
            } finally {
                CALLBACKS.finishBroadcast();
            }
        }
    }
}
