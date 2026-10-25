/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app.window;

import android.content.res.Configuration;
import android.graphics.Rect;

import com.gesher.app.DeviceProfile;

/**
 * The configuration of a task whose window the user resized (docs/lifecycle.md): what
 * Android's window manager gives a freeform task, as an override of the process (display)
 * configuration. Its screen size in dp is the window's client area (no system bars), its
 * smallest width the shorter side, its orientation landscape when it is wider than tall,
 * its screen layout size and long bits from those (Configuration.reduceScreenLayout), its
 * window configuration's bounds and app bounds the window's. Resources then resolve
 * size and orientation qualifiers ({@code -land}, {@code -w600dp}) from the window's size.
 * The window session puts it in the configuration ViewRootImpl gets (relayout, resized:
 * onConfigurationChanged for the changes an Activity handles), the system server in what it
 * launches and relaunches Activities of the task with (ActivityRelaunchItem for the changes
 * it does not handle).
 */
public final class TaskConfiguration {
    private TaskConfiguration() {
    }

    /** The override for a task {@code bounds} (pixels, from 0,0) in size. */
    public static Configuration override(Rect bounds) {
        Configuration base = DeviceProfile.configuration();
        Configuration c = new Configuration();
        int dpi = DeviceProfile.DENSITY_DPI;
        int w = Math.max(1, bounds.width() * 160 / dpi);
        int h = Math.max(1, bounds.height() * 160 / dpi);
        c.screenWidthDp = w;
        c.screenHeightDp = h;
        c.smallestScreenWidthDp = Math.min(w, h);
        c.orientation = w > h ? Configuration.ORIENTATION_LANDSCAPE
                : Configuration.ORIENTATION_PORTRAIT;
        c.screenLayout = Configuration.reduceScreenLayout(
                Configuration.resetScreenLayout(base.screenLayout), Math.max(w, h),
                Math.min(w, h));
        c.densityDpi = dpi;
        c.windowConfiguration.setBounds(bounds);
        c.windowConfiguration.setAppBounds(bounds);
        return c;
    }

    /** The process configuration with {@link #override} applied: what the Activity sees. */
    public static Configuration merged(Rect bounds) {
        Configuration c = DeviceProfile.configuration();
        if (bounds != null) c.updateFrom(override(bounds));
        return c;
    }
}
