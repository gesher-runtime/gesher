/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.graphics.Rect;
import android.util.MergedConfiguration;
import android.view.IWindow;
import android.view.IWindowSession;
import android.view.InputChannel;
import android.view.SurfaceControl;
import android.view.WindowManager;
import android.view.WindowManagerGlobal;
import android.view.WindowRelayoutResult;

import com.gesher.app.DeviceProfile;

/**
 * STAND-IN, for the graphics track to replace: the window session the system server
 * hands out while no real one is plugged in ({@link WindowManagerService#setSessionFactory}).
 * It lets an app's window be added headless, as handleResumeActivity adds the decor view:
 * {@code addToDisplayAsUser} answers ADD_OKAY with the display's frame, {@code relayout}
 * the same frames and no surface, {@code remove} nothing. No input channel is given and
 * nothing is ever drawn (the app gets no vsync either). Every other method answers
 * IWindowSession.Default's default (see {@link StandIn}).
 */
public final class StandInWindowSession extends IWindowSession.Default {
    private static void fill(WindowRelayoutResult result) {
        if (result == null) return;
        Rect display = new Rect(0, 0, DeviceProfile.WIDTH, DeviceProfile.HEIGHT);
        result.frames.frame.set(display);
        result.frames.displayFrame.set(display);
        result.frames.parentFrame.set(display);
        result.mergedConfiguration.setConfiguration(DeviceProfile.configuration(),
                new android.content.res.Configuration());
        result.insetsState.setDisplayFrame(display);
    }

    @Override
    public int addToDisplayAsUser(IWindow window, WindowManager.LayoutParams attrs,
            int viewVisibility, int layerStackId, int userId, int requestedVisibleTypes,
            InputChannel outInputChannel, WindowRelayoutResult result) {
        fill(result);
        SystemServer.log("window session (stand-in): added " + attrs.getTitle()
                + " (type " + attrs.type + ")");
        return WindowManagerGlobal.ADD_OKAY | WindowManagerGlobal.ADD_FLAG_APP_VISIBLE;
    }

    @Override
    public int addToDisplay(IWindow window, WindowManager.LayoutParams attrs,
            int viewVisibility, int layerStackId, int requestedVisibleTypes,
            InputChannel outInputChannel, WindowRelayoutResult result) {
        return addToDisplayAsUser(window, attrs, viewVisibility, layerStackId, 0,
                requestedVisibleTypes, outInputChannel, result);
    }

    @Override
    public int relayout(IWindow window, WindowManager.LayoutParams attrs, int requestedWidth,
            int requestedHeight, int viewVisibility, int flags, int seq, int lastSyncSeqId,
            WindowRelayoutResult outRelayoutResult, SurfaceControl outSurface) {
        fill(outRelayoutResult);
        return 0;
    }

    @Override
    public void remove(android.os.IBinder clientToken) {
        SystemServer.log("window session (stand-in): removed a window");
    }
}
