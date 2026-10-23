/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app;

import android.app.WindowConfiguration;
import android.content.res.Configuration;
import android.graphics.Rect;
import android.os.LocaleList;
import android.view.Display;
import android.view.DisplayInfo;
import android.view.Surface;

/**
 * The headless device the app sees: one display, a phone-sized portrait screen, en-US.
 * The display stand-in reports it, and the process configuration is derived from it, as
 * system_server derives it from DisplayManagerService and the window manager.
 */
public final class DeviceProfile {
    public static final int WIDTH = 1080;
    public static final int HEIGHT = 2400;
    public static final int DENSITY_DPI = 420;
    public static final float REFRESH_RATE = 60f;

    private DeviceProfile() {
    }

    public static int widthDp() {
        return WIDTH * 160 / DENSITY_DPI;
    }

    public static int heightDp() {
        return HEIGHT * 160 / DENSITY_DPI;
    }

    /** The default display, as DisplayManagerService would describe it. */
    public static DisplayInfo displayInfo() {
        DisplayInfo di = new DisplayInfo();
        di.displayId = Display.DEFAULT_DISPLAY;
        di.layerStack = 0;
        di.type = Display.TYPE_INTERNAL;
        di.name = "Gesher headless display";
        di.uniqueId = "local:0";
        di.appWidth = WIDTH;
        di.appHeight = HEIGHT;
        di.logicalWidth = WIDTH;
        di.logicalHeight = HEIGHT;
        di.smallestNominalAppWidth = WIDTH;
        di.smallestNominalAppHeight = WIDTH;
        di.largestNominalAppWidth = HEIGHT;
        di.largestNominalAppHeight = HEIGHT;
        di.rotation = Surface.ROTATION_0;
        di.installOrientation = Surface.ROTATION_0;
        Display.Mode mode = new Display.Mode(1, WIDTH, HEIGHT, REFRESH_RATE);
        di.modeId = mode.getModeId();
        di.defaultModeId = mode.getModeId();
        di.supportedModes = new Display.Mode[] {mode};
        di.appsSupportedModes = new Display.Mode[] {mode};
        di.renderFrameRate = REFRESH_RATE;
        di.supportedRefreshRates = new float[] {REFRESH_RATE};
        di.logicalDensityDpi = DENSITY_DPI;
        di.physicalXDpi = DENSITY_DPI;
        di.physicalYDpi = DENSITY_DPI;
        di.presentationDeadlineNanos = (long) (1_000_000_000L / REFRESH_RATE);
        di.state = Display.STATE_ON;
        di.committedState = Display.STATE_ON;
        di.brightnessMinimum = 0f;
        di.brightnessMaximum = 1f;
        di.brightnessDefault = 0.5f;
        di.brightnessDim = 0.05f;
        return di;
    }

    /** The process configuration, as ActivityThread gets it in bindApplication. */
    public static Configuration configuration() {
        Configuration c = new Configuration();
        c.setToDefaults();
        c.setLocales(LocaleList.forLanguageTags("en-US"));
        c.densityDpi = DENSITY_DPI;
        c.screenWidthDp = widthDp();
        c.screenHeightDp = heightDp();
        c.smallestScreenWidthDp = widthDp();
        c.orientation = Configuration.ORIENTATION_PORTRAIT;
        c.screenLayout = Configuration.SCREENLAYOUT_SIZE_NORMAL
                | Configuration.SCREENLAYOUT_LONG_YES
                | Configuration.SCREENLAYOUT_LAYOUTDIR_LTR
                | Configuration.SCREENLAYOUT_ROUND_NO;
        c.uiMode = Configuration.UI_MODE_TYPE_NORMAL | Configuration.UI_MODE_NIGHT_NO;
        c.touchscreen = Configuration.TOUCHSCREEN_FINGER;
        c.keyboard = Configuration.KEYBOARD_NOKEYS;
        c.keyboardHidden = Configuration.KEYBOARDHIDDEN_YES;
        c.hardKeyboardHidden = Configuration.HARDKEYBOARDHIDDEN_YES;
        c.navigation = Configuration.NAVIGATION_NONAV;
        c.navigationHidden = Configuration.NAVIGATIONHIDDEN_YES;
        c.colorMode = Configuration.COLOR_MODE_WIDE_COLOR_GAMUT_NO
                | Configuration.COLOR_MODE_HDR_NO;
        Rect bounds = new Rect(0, 0, WIDTH, HEIGHT);
        c.windowConfiguration.setBounds(bounds);
        c.windowConfiguration.setAppBounds(bounds);
        c.windowConfiguration.setMaxBounds(bounds);
        c.windowConfiguration.setWindowingMode(WindowConfiguration.WINDOWING_MODE_FULLSCREEN);
        c.windowConfiguration.setRotation(Surface.ROTATION_0);
        return c;
    }
}
