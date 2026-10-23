/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app.window;

import android.app.Activity;
import android.app.GesherActivityWindow;
import android.content.Context;
import android.os.ServiceManager;
import android.os.SystemProperties;

import com.android.internal.inputmethod.IInputMethodClient;
import com.android.internal.inputmethod.IRemoteInputConnection;
import com.android.internal.view.IInputMethodManager;
import com.gesher.app.DeviceProfile;
import com.gesher.server.StandIn;

/**
 * gesher-app's window mode ({@code --window}, {@code --hidden-window}): the launched
 * Activity's UI in a Win32 window until the window is closed (docs/graphics.md).
 */
public final class WindowMode {
    /** The display a window gets when none is given: 400x800 dp at 160 dpi (1 dp = 1 px). */
    public static final String DEFAULT_DISPLAY = "400x800@160";

    private WindowMode() {
    }

    /**
     * Before the process configuration is made: the display is {@code display}
     * ({@code WxH[@dpi]}), else what {@code gesher.display}/{@code GESHER_DISPLAY} say,
     * else {@link #DEFAULT_DISPLAY} (a phone-sized window rather than a 1080x2400 one).
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
        // ViewRootImpl gets InputMethodManager's instance, which needs input_method. The
        // system services may serve one (com.gesher.server.Services); if not, a stand-in
        // with no input method is added here.
        if (ServiceManager.checkService(Context.INPUT_METHOD_SERVICE) == null) {
            ServiceManager.addService(Context.INPUT_METHOD_SERVICE,
                    StandIn.of(IInputMethodManager.class, new NoInputMethod()).asBinder());
        }
        return WindowSession.install(show);
    }

    /** input_method with no input method: clients are accepted, nothing is shown. */
    static final class NoInputMethod extends IInputMethodManager.Default {
        @Override
        public void addClient(IInputMethodClient client, IRemoteInputConnection inputConnection,
                int untrustedDisplayId) {
        }
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
