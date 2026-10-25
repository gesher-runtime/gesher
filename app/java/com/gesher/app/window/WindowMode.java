/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app.window;

import android.app.Activity;
import android.app.GesherActivityWindow;
import android.os.SystemProperties;

import com.gesher.app.DeviceProfile;

/**
 * gesher-app's window mode ({@code --window}, {@code --hidden-window}): the launched
 * Activity's UI in a Win32 window until the window is closed (docs/graphics.md).
 */
public final class WindowMode {
    /** The display a window gets when none is given: 400x800 dp at 160 dpi (1 dp = 1 px). */
    public static final String DEFAULT_DISPLAY = "400x800@160";

    /** An app process's windows are Win32 windows, shown once drawn. */
    public static final String WINDOW = "window";
    /** The same, never shown (tests read them back with PrintWindow). */
    public static final String HIDDEN = "hidden";
    /** No Win32 windows: windows are laid out headless, with no surface and no renderer. */
    public static final String HEADLESS = "headless";
    /**
     * The environment variable that gives an app process its window mode ({@link #WINDOW},
     * {@link #HIDDEN} or {@link #HEADLESS}): the system server sets it for the processes it
     * starts (what the launcher asked for the package: gesher-app's --window,
     * --hidden-window, --headless), else they inherit the device's.
     */
    public static final String ENV = "GESHER_APP_WINDOW";

    /** {@code mode} if it is one ({@code hidden-window} means hidden), else null. */
    public static String parse(String mode) {
        if (mode == null) return null;
        switch (mode.trim().toLowerCase(java.util.Locale.ROOT)) {
            case WINDOW:
            case "shown":
                return WINDOW;
            case HIDDEN:
            case "hidden-window":
                return HIDDEN;
            case HEADLESS:
            case "none":
                return HEADLESS;
            default:
                return null;
        }
    }

    /** The window mode {@link #ENV} gives, else {@code fallback}. */
    public static String fromEnvironment(String fallback) {
        String mode = parse(System.getenv(ENV));
        return mode != null ? mode : fallback;
    }

    private WindowMode() {
    }

    /**
     * Before the process configuration is made: the display is {@code display}
     * ({@code WxH[@dpi]}), else what {@code gesher.display}/{@code GESHER_DISPLAY} say,
     * else {@link #DEFAULT_DISPLAY} (DeviceProfile's default too).
     */
    public static void setUpDisplay(String display) {
        if (display != null) {
            DeviceProfile.parseAndSet(display);
        } else if (empty(System.getProperty("gesher.display"))
                && empty(System.getenv("GESHER_DISPLAY"))) {
            DeviceProfile.parseAndSet(DEFAULT_DISPLAY);
        }
    }

    /**
     * Installs the window session ({@link WindowSession#install}). Choreographer schedules
     * frames on the message queue's clock (there is no display's vsync to receive), unless
     * {@code debug.choreographer.vsync} was set: before Choreographer is first used.
     */
    public static WindowSession install(boolean show) {
        if (SystemProperties.get("debug.choreographer.vsync").isEmpty()) {
            SystemProperties.set("debug.choreographer.vsync", "false");
        }
        // ViewRootImpl gets InputMethodManager's instance, which needs input_method: the
        // process's windows get Windows' text input (input track, ADR 0039).
        com.gesher.app.input.WindowsInputMethod.install();
        WindowSession session = WindowSession.install(show);
        // Toasts are this process's popups too (no SystemUI draws them).
        ToastHost.install();
        return session;
    }

    /**
     * Shows a resumed Activity's window (handleResumeActivity's part). Closing it finishes
     * and destroys the Activity, removes the window, then runs {@code onClosed}.
     */
    public static void show(Activity activity, Runnable onClosed) {
        WindowSession session = WindowSession.get();
        if (session == null) {
            throw new IllegalStateException("WindowMode.install first");
        }
        String name = activity.getComponentName().getClassName();
        session.setCloseHandler((s, window, attrs) -> {
            if (attrs.token != null && attrs.token != activity.getActivityToken()) {
                return;  // another window (a dialog): only the Activity's ends the app
            }
            GesherActivityWindow.finishAndDestroy(activity);
            log(name + " finished, destroyed");
            onClosed.run();
        });
        // Minimizing the window stops the Activity, restoring it resumes it
        // (docs/lifecycle.md): the single process has no system server to move its task.
        session.setLifecycleHandler(new WindowSession.LifecycleHandler() {
            @Override
            public void onTaskVisibility(android.os.IBinder token, int taskId, boolean visible) {
                if (token != activity.getActivityToken()) return;
                if (visible) {
                    GesherActivityWindow.restart(activity);
                    log(name + " restarted, resumed (its window was restored)");
                } else {
                    android.os.Bundle state = GesherActivityWindow.stop(activity);
                    log(name + " paused, stopped (its window was minimized)"
                            + (state != null ? ", saved state of " + state.size() + " keys" : ""));
                }
            }

            @Override
            public void onTaskActivated(android.os.IBinder token, int taskId) {
            }
        });
        GesherActivityWindow.makeVisible(activity);
        log(name + " window added");
    }

    private static boolean empty(String s) {
        return s == null || s.isEmpty();
    }

    private static void log(String message) {
        System.out.println("gesher-app: " + message);
        System.out.flush();
    }
}
