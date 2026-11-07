/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.windowpromotion.target;

import android.app.Activity;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.os.Bundle;
import android.os.Handler;
import android.os.Process;
import android.util.ArrayMap;
import android.view.View;
import java.lang.reflect.Field;

/** Samples only its own real native window and verifies a presented synthetic frame. */
public class TargetActivity extends Activity {
    static volatile TargetActivity primary;
    private static final int[] COLORS = {0xffa51f3d, 0xff137d43, 0xff154da8, 0xffd6b72f};
    private int attempts;
    private static Object field(Object owner, String name) throws Exception {
        Field value = owner.getClass().getDeclaredField(name);
        value.setAccessible(true);
        return value.get(owner);
    }
    private static boolean quadrants(int[] pixels) {
        if (pixels == null || pixels.length < 3 || pixels[0] < 80 || pixels[1] < 80) return false;
        int w = pixels[0], h = pixels[1];
        if (pixels.length != 2 + w * h) return false;
        for (int i = 0; i < 4; i++) {
            int x = (i % 2 == 0 ? 1 : 3) * w / 4;
            int y = (i < 2 ? 1 : 3) * h / 4;
            if (pixels[2 + y * w + x] != COLORS[i]) return false;
        }
        return true;
    }
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        boolean background = getClass() == BackgroundActivity.class;
        if (!background) primary = this;
        setTitle(background ? "Gesher unapproved promotion fixture" : "Gesher window promotion fixture");
        System.out.println("promotion: " + (background ? "unapproved-created" : "target-created") + " pid=" + Process.myPid()
                + " service=" + (KeepAliveService.instance != null));
        setContentView(new View(this) {
            final Paint paint = new Paint();
            @Override protected void onDraw(Canvas canvas) {
                int w = getWidth(), h = getHeight();
                for (int i = 0; i < 4; i++) {
                    paint.setColor(COLORS[i]);
                    canvas.drawRect((i % 2) * w / 2f, (i / 2) * h / 2f,
                            (i % 2 + 1) * w / 2f, (i / 2 + 1) * h / 2f, paint);
                }
                paint.setColor(0xffffffff);
                paint.setTextSize(22);
                canvas.drawText("Synthetic window promotion", 12, 32, paint);
            }
        });
        new Handler().postDelayed(this::report, 250);
    }
    private void report() {
        try {
            Class<?> global = Class.forName("android.view.WindowManagerGlobal");
            Object session = global.getMethod("getWindowSession").invoke(null);
            if (!session.getClass().getName().equals("com.gesher.app.window.WindowSession")) {
                if (++attempts == 1) System.out.println("promotion: no-native-window-session");
            } else {
                ArrayMap<?, ?> windows = (ArrayMap<?, ?>)field(session, "mWindows");
                synchronized (windows) {
                    for (int i = 0; i < windows.size(); i++) {
                        Object window = field(windows.valueAt(i), "nativeWindow");
                        if (window == null) continue;
                        long frames = ((Number)window.getClass().getMethod("getFramesPresented").invoke(window)).longValue();
                        if (frames < 1) continue;
                        boolean back = quadrants((int[])window.getClass().getMethod("readBackBuffer").invoke(window));
                        boolean capture = quadrants((int[])window.getClass().getMethod("capture").invoke(window));
                        if (!back || !capture) continue;
                        if (KeepAliveService.instance == null) throw new AssertionError("Original service missing");
                        System.out.println("promotion: " + (getClass() == BackgroundActivity.class ? "unapproved-frame-pass" : "frame-pass")
                                + " pid=" + Process.myPid() + " service=true frames=" + frames
                                + " back=true capture=true hardware=" + getWindow().getDecorView().isHardwareAccelerated());
                        return;
                    }
                }
                ++attempts;
            }
        } catch (Throwable error) {
            System.out.println("promotion: frame-error " + error.getClass().getSimpleName());
            ++attempts;
        }
        if (attempts < 80) new Handler().postDelayed(this::report, 250);
        else System.out.println("promotion: frame-timeout");
    }
    @Override protected void onDestroy() {
        if (primary == this) {
            primary = null;
            System.out.println("promotion: first-activity-destroyed pid=" + Process.myPid());
        }
        super.onDestroy();
    }
}
