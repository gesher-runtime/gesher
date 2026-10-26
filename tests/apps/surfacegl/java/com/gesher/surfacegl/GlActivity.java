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
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.widget.FrameLayout;

/**
 * EGL/GLES 2 into a SurfaceView from native code (lib/x86_64/libsurfacegl.so), as SDL
 * draws (surfaces track, tests/windows/tracks/52-surfaces.ps1). A SurfaceView of
 * 400x400 at the window's top; its surface goes to the library when it changes, which
 * draws frames of a red left half and a green right half on a thread of its own.
 * "surfacegl: phase 1 ready" once frames came; then setFixedSize(100, 100) (the EGL
 * surface follows) and "surfacegl: phase 2 ready"; then the Activity finishes, which
 * destroys the surface and stops the library.
 */
public class GlActivity extends Activity {
    static native void start(Surface surface);

    static native void stop();

    static void say(String s) {
        System.out.println("surfacegl: " + s);
        System.out.flush();
    }

    private final Handler mMain = new Handler(Looper.getMainLooper());

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        System.loadLibrary("surfacegl");
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.WHITE);
        final SurfaceView view = new SurfaceView(this);
        view.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
                say("surfaceCreated");
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int w, int h) {
                say("surfaceChanged " + w + "x" + h);
                start(holder.getSurface());
            }

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
                say("surfaceDestroyed");
                stop();
            }
        });
        root.addView(view, new FrameLayout.LayoutParams(400, 400));
        setContentView(root);
        mMain.postDelayed(() -> {
            say("phase 1 ready");
            mMain.postDelayed(() -> {
                view.getHolder().setFixedSize(100, 100);
                mMain.postDelayed(() -> {
                    say("phase 2 ready");
                    mMain.postDelayed(() -> {
                        say("finishing");
                        finish();
                    }, 6000);
                }, 4000);
            }, 6000);
        }, 8000);
    }

    @Override
    protected void onPause() {
        super.onPause();
        say("onPause");
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        say("onDestroy");
    }
}
