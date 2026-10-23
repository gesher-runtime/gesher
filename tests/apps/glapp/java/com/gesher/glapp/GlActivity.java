/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.glapp;

import android.app.Activity;
import android.os.Bundle;
import android.view.Surface;

import java.lang.reflect.Method;

/**
 * Draws into windows from native code (lib/x86_64/libglapp.so). An Activity
 * has no window of its own under gesher-app yet, so this one makes them with
 * gesher-app's com.gesher.app.window.NativeWindow (by reflection: the APK is
 * built against the public SDK) and hands their Surfaces to the library:
 * ANativeWindow_fromSurface + ANativeWindow_lock into one window, which is
 * then read back with PrintWindow and checked here; EGL/GLES 3 and 2 with
 * eglCreateWindowSurface into another, checked by the library with
 * glReadPixels. onResume prints the results.
 */
public class GlActivity extends Activity {
    static native String cpuDraw(Surface surface);

    static native String glDraw(Surface surface, int es);

    private String cpu = "not run";
    private String cpuReadBack = "not run";
    private String es3 = "not run";
    private String es2 = "not run";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        System.loadLibrary("glapp");
        try {
            Class<?> nw = Class.forName("com.gesher.app.window.NativeWindow");
            Method create = nw.getMethod("create", String.class, int.class, int.class);
            Method show = nw.getMethod("show");
            Method getSurface = nw.getMethod("getSurface");
            Method capture = nw.getMethod("capture");
            Method close = nw.getMethod("close");

            Object window = create.invoke(null, "glapp: ANativeWindow_lock", 160, 96);
            show.invoke(window);
            Surface surface = (Surface) getSurface.invoke(window);
            cpu = cpuDraw(surface);
            cpuReadBack = checkQuadrants((int[]) capture.invoke(window));
            surface.release();
            close.invoke(window);

            Object glWindow = create.invoke(null, "glapp: EGL", 160, 96);
            show.invoke(glWindow);
            Surface glSurface = (Surface) getSurface.invoke(glWindow);
            es3 = glDraw(glSurface, 3);
            es2 = glDraw(glSurface, 2);
            glSurface.release();
            close.invoke(glWindow);
        } catch (ReflectiveOperationException e) {
            cpu = "FAIL: " + e;
        }
    }

    /** The PrintWindow read-back {width, height, ARGB...}: four colored quadrants. */
    static String checkQuadrants(int[] capture) {
        if (capture == null) {
            return "FAIL: PrintWindow failed";
        }
        int w = capture[0], h = capture[1];
        int[] colors = {0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff};
        int wrong = 0;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                int quadrant = (y >= h / 2 ? 2 : 0) + (x >= w / 2 ? 1 : 0);
                if (capture[2 + y * w + x] != colors[quadrant]) {
                    wrong++;
                }
            }
        }
        return (wrong == 0 ? "ok: " : "FAIL: ") + w + "x" + h + ", " + wrong
                + " pixels not red/green/blue/white by quadrant";
    }

    @Override
    protected void onResume() {
        super.onResume();
        System.out.println("GlActivity cpu: " + cpu);
        System.out.println("GlActivity cpu read back: " + cpuReadBack);
        System.out.println("GlActivity es3: " + es3);
        System.out.println("GlActivity es2: " + es2);
        System.out.flush();
    }
}
