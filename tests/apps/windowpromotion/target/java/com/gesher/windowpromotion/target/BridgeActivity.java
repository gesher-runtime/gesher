/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.windowpromotion.target;

import android.app.Activity;
import android.app.Dialog;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.os.Bundle;
import android.os.Handler;
import android.os.Process;
import android.util.ArrayMap;
import android.view.View;
import android.view.WindowManager;
import java.lang.reflect.Field;

/** A transparent empty routing Activity whose real dialog precedes its base window. */
public final class BridgeActivity extends Activity {
    static BridgeActivity instance;
    private int attempts;
    private static Object field(Object owner, String name) throws Exception {
        Field value = owner.getClass().getDeclaredField(name);
        value.setAccessible(true);
        return value.get(owner);
    }
    private View content() {
        return new View(this) {
            private final Paint paint = new Paint();
            @Override protected void onDraw(Canvas canvas) {
                paint.setColor(0xff137d43);
                canvas.drawRect(0, 0, getWidth(), getHeight(), paint);
                paint.setColor(0xff154da8);
                canvas.drawRect(0, getHeight() / 2, getWidth(), getHeight(), paint);
            }
        };
    }
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        instance = this;
        setTitle("Gesher empty bridge fixture");
        // Dialog themes can leave decor padding around an empty content frame.
        // This reproduces the real 32-pixel bridge without adding any content.
        getWindow().getDecorView().setPadding(10, 16, 10, 16);
        Dialog dialog = new Dialog(this, android.R.style.Theme_Material_Light_NoActionBar);
        dialog.setTitle("Gesher bridge dialog fixture");
        dialog.setContentView(content());
        dialog.show();
        dialog.getWindow().setLayout(WindowManager.LayoutParams.MATCH_PARENT,
                WindowManager.LayoutParams.MATCH_PARENT);
        System.out.println("bridge: created pid=" + Process.myPid());
        new Handler().postDelayed(this::report, 400);
    }
    void revealContent() {
        setContentView(content());
        getWindow().setLayout(240, 180);
        System.out.println("bridge: content-added pid=" + Process.myPid());
    }
    private void report() {
        try {
            Object session = Class.forName("android.view.WindowManagerGlobal")
                    .getMethod("getWindowSession").invoke(null);
            ArrayMap<?, ?> windows = (ArrayMap<?, ?>)field(session, "mWindows");
            synchronized (windows) {
                for (int i = 0; i < windows.size(); i++) {
                    Object window = field(windows.valueAt(i), "nativeWindow");
                    if (window == null) continue;
                    int[] pixels = (int[])window.getClass().getMethod("capture").invoke(window);
                    if (pixels == null || pixels[0] < 80 || pixels[1] < 80) continue;
                    int width = pixels[0], height = pixels[1];
                    if (pixels[2 + (height / 4) * width + width / 2] != 0xff137d43
                            || pixels[2 + (height * 3 / 4) * width + width / 2] != 0xff154da8) continue;
                    System.out.println("bridge: frame-pass pid=" + Process.myPid());
                    return;
                }
            }
        } catch (Throwable error) {
            System.out.println("bridge: frame-error " + error.getClass().getSimpleName());
        }
        if (++attempts < 80) new Handler().postDelayed(this::report, 100);
    }
    @Override protected void onDestroy() { instance = null; super.onDestroy(); }
}
