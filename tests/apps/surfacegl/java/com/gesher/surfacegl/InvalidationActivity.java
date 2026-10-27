/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.surfacegl;

import android.app.Activity;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.widget.FrameLayout;

/** Only the native producer changes colors; no input, layout change or View.invalidate. */
public final class InvalidationActivity extends Activity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        System.loadLibrary("surfacegl");
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.WHITE);
        SurfaceView view = new SurfaceView(this);
        view.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override public void surfaceCreated(SurfaceHolder holder) { }

            @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                GlActivity.startPhases(holder.getSurface());
            }

            @Override public void surfaceDestroyed(SurfaceHolder holder) {
                GlActivity.stop();
                GlActivity.say("invalidation: surface destroyed");
            }
        });
        root.addView(view, new FrameLayout.LayoutParams(400, 400));
        setContentView(root);
        new Handler(Looper.getMainLooper()).postDelayed(this::finish, 30000);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        GlActivity.say("invalidation: done");
    }
}
