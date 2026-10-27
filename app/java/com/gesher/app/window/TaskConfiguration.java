/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app.window;

import android.content.pm.ActivityInfo;
import android.content.res.Configuration;
import android.graphics.Rect;
import android.view.Surface;

import com.gesher.app.DeviceProfile;

/**
 * The configuration of a task whose window the user resized (docs/lifecycle.md), or whose
 * top Activity asked for an orientation the display does not have (docs/windows3.md): what
 * Android's window manager gives a freeform task, or a task on a device that rotates, as an
 * override of the process (display) configuration. Its screen size in dp is the window's
 * client area (no system bars), its smallest width the shorter side, its orientation
 * landscape when it is wider than tall, its screen layout size and long bits from those
 * (Configuration.reduceScreenLayout), its window configuration's bounds and app bounds the
 * window's. Resources then resolve size and orientation qualifiers ({@code -land},
 * {@code -w600dp}) from the window's size. A rotated task also has the display's rotation
 * and its rotated size (the window configuration's display rotation, rotation and max
 * bounds), so the Activity's {@code Display.getRotation()}, {@code getSize},
 * {@code getRealSize} and {@code WindowManager.getMaximumWindowMetrics()} are those of the
 * rotated display, as on a phone turned on its side.
 *
 * <p>The window session puts it in the configuration ViewRootImpl gets (relayout, resized:
 * onConfigurationChanged for the changes an Activity handles), the system server in what it
 * launches and relaunches Activities of the task with (ActivityRelaunchItem for the changes
 * it does not handle).
 */
public final class TaskConfiguration {
    private TaskConfiguration() {
    }

    /** The override for a task {@code bounds} (pixels, from 0,0) in size. */
    public static Configuration override(Rect bounds) {
        return override(bounds, Surface.ROTATION_0);
    }

    /**
     * The override for a task {@code bounds} (pixels, from 0,0) in size on the display
     * turned to {@code rotation} (Surface.ROTATION_*; 0 for none).
     */
    public static Configuration override(Rect bounds, int rotation) {
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
                Math.min(w, h)) & ~Configuration.SCREENLAYOUT_LAYOUTDIR_MASK;
        // A task overrides geometry, not the package's locale. Copying the
        // display's LTR bits here erased Hebrew/Arabic direction at launch.
        c.densityDpi = dpi;
        c.windowConfiguration.setBounds(bounds);
        c.windowConfiguration.setAppBounds(bounds);
        // Zero is an actual task orientation too: an unfocused portrait task must
        // not inherit the shared Application display's landscape rotation.
        c.windowConfiguration.setRotation(rotation);
        c.windowConfiguration.setDisplayRotation(rotation);
        c.windowConfiguration.setMaxBounds(display(rotation));
        return c;
    }

    /** The process configuration with {@link #override} applied: what the Activity sees. */
    public static Configuration merged(Rect bounds) {
        Configuration c = DeviceProfile.configuration();
        if (bounds != null) c.updateFrom(override(bounds));
        return c;
    }

    // --- orientation (windows3 track) -----------------------------------------------------

    /** The display's bounds turned to {@code rotation}: its sides swapped at 90 and 270. */
    public static Rect display(int rotation) {
        boolean swap = rotation == Surface.ROTATION_90 || rotation == Surface.ROTATION_270;
        return new Rect(0, 0, swap ? DeviceProfile.HEIGHT : DeviceProfile.WIDTH,
                swap ? DeviceProfile.WIDTH : DeviceProfile.HEIGHT);
    }

    /** Whether the display is wider than tall in its natural orientation (ROTATION_0). */
    public static boolean naturalLandscape() {
        return DeviceProfile.WIDTH > DeviceProfile.HEIGHT;
    }

    /** Whether {@code requested} asks for landscape (any of its kinds). */
    public static boolean isLandscape(int requested) {
        switch (requested) {
            case ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE:
            case ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE:
            case ActivityInfo.SCREEN_ORIENTATION_USER_LANDSCAPE:
            case ActivityInfo.SCREEN_ORIENTATION_REVERSE_LANDSCAPE:
                return true;
            default:
                return false;
        }
    }

    /** Whether {@code requested} asks for portrait (any of its kinds). */
    public static boolean isPortrait(int requested) {
        switch (requested) {
            case ActivityInfo.SCREEN_ORIENTATION_PORTRAIT:
            case ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT:
            case ActivityInfo.SCREEN_ORIENTATION_USER_PORTRAIT:
            case ActivityInfo.SCREEN_ORIENTATION_REVERSE_PORTRAIT:
                return true;
            default:
                return false;
        }
    }

    /**
     * DisplayRotation.rotationForOrientation on a device that rotates but has no sensor (a
     * desktop): the rotation the display takes for an Activity asking for {@code requested}
     * (ActivityInfo.SCREEN_ORIENTATION_*), the display being at {@code current}. Landscape
     * on a portrait display is ROTATION_90 (reverse landscape ROTATION_270), portrait on a
     * landscape display ROTATION_90 (reverse portrait ROTATION_270), the natural orientation
     * ROTATION_0 (its reverse ROTATION_180); sensor landscape or portrait keeps the current
     * rotation when it is of the kind asked for; {@code locked} keeps the current one; the
     * rest (unspecified, user, sensor, full sensor, nosensor) is the natural orientation, as
     * the user's rotation with no sensor is. {@code behind} is resolved by the caller.
     */
    public static int rotationFor(int requested, int current) {
        boolean natLand = naturalLandscape();
        boolean currentLand = (current == Surface.ROTATION_90 || current == Surface.ROTATION_270)
                != natLand;
        switch (requested) {
            case ActivityInfo.SCREEN_ORIENTATION_LOCKED:
                return current;
            case ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE:
            case ActivityInfo.SCREEN_ORIENTATION_USER_LANDSCAPE:
                if (currentLand) return current;
                return natLand ? Surface.ROTATION_0 : Surface.ROTATION_90;
            case ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE:
                return natLand ? Surface.ROTATION_0 : Surface.ROTATION_90;
            case ActivityInfo.SCREEN_ORIENTATION_REVERSE_LANDSCAPE:
                return natLand ? Surface.ROTATION_180 : Surface.ROTATION_270;
            case ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT:
            case ActivityInfo.SCREEN_ORIENTATION_USER_PORTRAIT:
                if (!currentLand) return current;
                return natLand ? Surface.ROTATION_90 : Surface.ROTATION_0;
            case ActivityInfo.SCREEN_ORIENTATION_PORTRAIT:
                return natLand ? Surface.ROTATION_90 : Surface.ROTATION_0;
            case ActivityInfo.SCREEN_ORIENTATION_REVERSE_PORTRAIT:
                return natLand ? Surface.ROTATION_270 : Surface.ROTATION_180;
            default:
                return Surface.ROTATION_0;
        }
    }

    /** "ROTATION_90"-style name of a rotation, for logs. */
    public static String rotationName(int rotation) {
        return "ROTATION_" + (rotation * 90);
    }
}
