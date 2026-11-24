/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.ActivityThread;
import android.content.Context;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.graphics.Rect;
import android.os.IBinder;
import android.os.Looper;
import android.os.ServiceManager;
import android.view.DisplayInfo;
import android.view.Surface;
import android.view.ViewConfiguration;

import com.gesher.app.DeviceProfile;
import com.gesher.app.window.TaskConfiguration;
import com.gesher.server.Services;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.util.Collections;
import java.util.Map;

/**
 * A window is never larger than its display (issue #426): whatever size the user gives a
 * task's window, the bounds the window session and the system server use
 * (TaskConfiguration.clampToDisplay, TaskConfiguration.override) lie within the display turned
 * as the task is, and the framework's real ViewConfiguration, built from a real ContextImpl
 * with that task configuration, allows a software layer (drawing cache) of the whole window.
 * Before the fix a 1920x941 window on the 400x800 display needed 6293408 bytes of the
 * 1280000 the drawing cache allowed, and a View of that size was not drawn.
 */
public final class WindowDisplayBoundsTest {
    private static int checks;

    private static void check(boolean condition, String reason) {
        if (!condition) throw new AssertionError(reason);
        checks++;
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        if (Looper.myLooper() == null) Looper.prepareMainLooper();
        DeviceProfile.set(400, 800, 160);  // Gesher's default display
        Map<String, IBinder> created = Services.create(Collections.emptyList());
        Field cacheField = ServiceManager.class.getDeclaredField("sCache");
        cacheField.setAccessible(true);
        @SuppressWarnings("unchecked")
        Map<String, IBinder> cache = (Map<String, IBinder>) cacheField.get(null);
        cache.putAll(created);
        Constructor<ActivityThread> threadConstructor = ActivityThread.class.getDeclaredConstructor();
        threadConstructor.setAccessible(true);
        Context system = threadConstructor.newInstance().getSystemContext();
        Constructor<ViewConfiguration> viewConfiguration =
                ViewConfiguration.class.getDeclaredConstructor(Context.class);
        viewConfiguration.setAccessible(true);

        // Sizes a user can give the Win32 window: the issue's maximized 1920x941, wider
        // than the display, larger both ways, within it, and the display itself.
        int[][] requests = {{1920, 941}, {640, 400}, {600, 700}, {420, 620}, {300, 500},
                {400, 800}, {800, 400}, {16384, 16384}};
        int[] rotations = {Surface.ROTATION_0, Surface.ROTATION_90, Surface.ROTATION_180,
                Surface.ROTATION_270};
        long largestWindowBytes = 0;
        for (int rotation : rotations) {
            Rect display = TaskConfiguration.display(rotation);
            for (int[] request : requests) {
                String what = request[0] + "x" + request[1] + " at "
                        + TaskConfiguration.rotationName(rotation);
                Rect within = TaskConfiguration.clampToDisplay(request[0], request[1], rotation);
                check(within.left == 0 && within.top == 0
                        && within.width() == Math.min(request[0], display.width())
                        && within.height() == Math.min(request[1], display.height()),
                        what + ": clamped to " + within + " on display " + display);
                // The override the window session and the system server give the task,
                // even for the unclamped request.
                for (Rect asked : new Rect[] {within, new Rect(0, 0, request[0], request[1])}) {
                    Configuration override = TaskConfiguration.override(asked, rotation);
                    Rect bounds = override.windowConfiguration.getBounds();
                    Rect app = override.windowConfiguration.getAppBounds();
                    Rect max = override.windowConfiguration.getMaxBounds();
                    check(max.equals(display), what + ": max bounds " + max + " are not the display "
                            + display);
                    check(bounds.equals(within) && app.equals(within) && max.contains(bounds),
                            what + ": task bounds " + bounds + ", app " + app + " outside max " + max);
                    check(override.screenWidthDp * DeviceProfile.DENSITY_DPI / 160 <= display.width()
                            && override.screenHeightDp * DeviceProfile.DENSITY_DPI / 160
                                    <= display.height(),
                            what + ": screen " + override.screenWidthDp + "x"
                                    + override.screenHeightDp + " dp larger than the display");
                }
                // The display the app reads (DisplayInfo, turned as the task is) holds it.
                DisplayInfo info = DeviceProfile.displayInfo();
                boolean swap = rotation == Surface.ROTATION_90 || rotation == Surface.ROTATION_270;
                int logicalWidth = swap ? info.logicalHeight : info.logicalWidth;
                int logicalHeight = swap ? info.logicalWidth : info.logicalHeight;
                check(within.width() <= logicalWidth && within.height() <= logicalHeight,
                        what + ": window " + within + " larger than the display "
                                + logicalWidth + "x" + logicalHeight);

                // The real ViewConfiguration of an Activity with that task configuration.
                Configuration merged = DeviceProfile.configuration();
                merged.updateFrom(TaskConfiguration.override(within, rotation));
                Context activity = system.createConfigurationContext(merged);
                int drawingCache = viewConfiguration.newInstance(activity)
                        .getScaledMaximumDrawingCacheSize();
                long windowBytes = 4L * within.width() * within.height();
                largestWindowBytes = Math.max(largestWindowBytes, windowBytes);
                check(windowBytes <= drawingCache, what + ": a View filling the " + within.width()
                        + "x" + within.height() + " window needs " + windowBytes
                        + " bytes, the drawing cache allows " + drawingCache);
            }
        }
        // The process's ViewConfiguration is made once (per density and device) from the first
        // context that asks; it must allow the largest window of any rotation.
        Configuration first = DeviceProfile.configuration();
        first.updateFrom(TaskConfiguration.override(TaskConfiguration.display(Surface.ROTATION_0),
                Surface.ROTATION_0));
        int shared = ViewConfiguration.get(system.createConfigurationContext(first))
                .getScaledMaximumDrawingCacheSize();
        check(shared == 4 * DeviceProfile.WIDTH * DeviceProfile.HEIGHT,
                "the drawing cache " + shared + " is not 4 bytes a display pixel");
        check(largestWindowBytes <= shared, "the largest window needs " + largestWindowBytes
                + " bytes, the shared drawing cache allows " + shared);
        // Counterfactual: the issue's unclamped 1920x941 window does not fit the cache, so the
        // clamping above is what keeps such a View drawn.
        check(4L * 1920 * 941 > shared, "1920x941 would fit a " + shared + "-byte cache");
        System.out.println("WindowDisplayBoundsTest all passed: " + checks);
        // Services.create owns periodic workers; the isolated fixture has no app event loop.
        System.exit(0);
    }
}
