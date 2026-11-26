/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.surfaceapp;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Intent;
import android.content.ServiceConnection;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.graphics.SurfaceTexture;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Parcel;
import android.os.Process;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.TextureView;
import android.widget.FrameLayout;

import java.util.concurrent.Callable;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;

/** Real AVC output into two display consumers, local and across a separate process. */
public final class CodecScalingActivity extends Activity {
    private final Handler main = new Handler(Looper.getMainLooper());
    private final CountDownLatch ready = new CountDownLatch(2);
    private final CountDownLatch connected = new CountDownLatch(1);
    private FrameLayout root;
    private SurfaceView surfaceView;
    private TextureView textureView;
    private volatile Surface texture;
    private volatile IBinder remote;
    private boolean bound;
    private ScalingProducer local;
    private final ServiceConnection connection = new ServiceConnection() {
        @Override public void onServiceConnected(ComponentName name, IBinder service) {
            remote = service; connected.countDown();
        }
        @Override public void onServiceDisconnected(ComponentName name) { remote = null; }
    };

    private static void say(String text) {
        System.out.println("scaling: " + text);
        System.out.flush();
    }

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        root = new FrameLayout(this);
        root.setBackgroundColor(Color.WHITE);
        surfaceView = new SurfaceView(this);
        surfaceView.getHolder().addCallback(new SurfaceHolder.Callback() {
            private boolean reported;
            @Override public void surfaceCreated(SurfaceHolder holder) {}
            @Override public void surfaceDestroyed(SurfaceHolder holder) {}
            @Override public void surfaceChanged(SurfaceHolder holder, int format, int w, int h) {
                if (!reported && w == 96 && h == 96 && holder.getSurface().isValid()) {
                    reported = true; ready.countDown();
                }
            }
        });
        root.addView(surfaceView, SurfaceActivity.at(0, 0, 96, 96));
        addTexture(ready);
        setContentView(root);
        new Thread(this::exercise, "codec-scaling-test").start();
    }

    private void addTexture(CountDownLatch latch) {
        textureView = new TextureView(this);
        textureView.setSurfaceTextureListener(new TextureView.SurfaceTextureListener() {
            @Override public void onSurfaceTextureAvailable(SurfaceTexture st, int w, int h) {
                texture = new Surface(st);
                latch.countDown();
            }
            @Override public void onSurfaceTextureSizeChanged(SurfaceTexture st, int w, int h) {}
            @Override public boolean onSurfaceTextureDestroyed(SurfaceTexture st) { return true; }
            @Override public void onSurfaceTextureUpdated(SurfaceTexture st) {}
        });
        root.addView(textureView, SurfaceActivity.at(120, 0, 96, 96));
    }

    private <T> T ui(Callable<T> action) throws Exception {
        FutureTask<T> task = new FutureTask<>(action);
        main.post(task);
        return task.get(10, TimeUnit.SECONDS);
    }

    private int call(int code, int mode, Surface replacement) throws Exception {
        IBinder binder = remote;
        ScalingProducer.require(binder != null, "producer service disconnected");
        Parcel data = Parcel.obtain(), reply = Parcel.obtain();
        try {
            data.writeInterfaceToken(ScalingService.DESCRIPTOR);
            if (code == ScalingService.OPEN) {
                surfaceView.getHolder().getSurface().writeToParcel(data, 0);
                texture.writeToParcel(data, 0);
            } else if (code == ScalingService.RENDER) data.writeInt(mode);
            else if (code == ScalingService.REPLACE) replacement.writeToParcel(data, 0);
            ScalingProducer.require(binder.transact(code, data, reply, 0), "service rejected transaction");
            reply.readException();
            return reply.readInt();
        } finally { reply.recycle(); data.recycle(); }
    }

    private void render(int mode) throws Exception {
        if (local != null) local.render(mode);
        else call(ScalingService.RENDER, mode, null);
    }

    private static boolean near(int value, int expected) {
        return Math.abs(Color.red(value) - Color.red(expected)) <= 40
                && Math.abs(Color.green(value) - Color.green(expected)) <= 40
                && Math.abs(Color.blue(value) - Color.blue(expected)) <= 40;
    }

    private boolean bitmapMatches(boolean cropped) {
        // Deliberately request a 3:1 bitmap while the view may be square. Android
        // getBitmap captures the displayed crop, then scales that result to this size.
        Bitmap bitmap = textureView.getBitmap(192, 64);
        if (bitmap == null) return false;
        try {
            for (int x : new int[]{16, 96, 176}) {
                int expected = cropped || x == 96 ? Color.GREEN : x < 96 ? Color.RED : Color.BLUE;
                for (int y : new int[]{12, 32, 52})
                    if (!near(bitmap.getPixel(x, y), expected)) return false;
            }
            return true;
        } finally { bitmap.recycle(); }
    }

    private void phase(int number, boolean cropped, String detail) throws Exception {
        long deadline = System.nanoTime() + 15_000_000_000L;
        boolean matches = false;
        while (System.nanoTime() < deadline) {
            if (ui(() -> bitmapMatches(cropped))) { matches = true; break; }
            Thread.sleep(40);
        }
        ScalingProducer.require(matches, "TextureView.getBitmap wrong pixels at phase " + number);
        say("phase " + number + " bitmap verified: " + detail);
        say("phase " + number + " ready");
        Thread.sleep(6_000); // Window harness captures actual SurfaceView and TextureView pixels.
    }

    private void exercise() {
        boolean passed = false;
        try {
            ScalingProducer.require(ready.await(30, TimeUnit.SECONDS), "output views unavailable");
            ScalingProducer.checkErrors();
            say("invalid modes and released state verified");
            boolean separate = getIntent().getBooleanExtra("scaling.remote", false);
            if (separate) {
                bound = ui(() -> bindService(new Intent(this, ScalingService.class), connection, BIND_AUTO_CREATE));
                ScalingProducer.require(bound && connected.await(30, TimeUnit.SECONDS), "producer service bind failed");
                int producerPid = call(ScalingService.OPEN, 0, null);
                ScalingProducer.require(producerPid != Process.myPid(), "producer was not in separate process");
                say("separate producer pid " + producerPid + " consumer pid " + Process.myPid());
            } else {
                local = new ScalingProducer(surfaceView.getHolder().getSurface(), texture);
                say("local producer pid " + Process.myPid());
            }
            render(0);
            phase(1, false, "default fit after no-Surface call");
            render(MediaCodecModes.CROP);
            phase(2, true, "live centered crop");
            ui(() -> {
                surfaceView.setLayoutParams(SurfaceActivity.at(0, 0, 192, 64));
                textureView.setLayoutParams(SurfaceActivity.at(208, 0, 192, 64));
                return null;
            });
            // No decoder input or output release: raw source pixels must still be available.
            phase(3, false, "resize same frame, no new decoded input");
            CountDownLatch replaced = new CountDownLatch(1);
            Surface oldTexture = texture;
            try {
                ui(() -> {
                    surfaceView.setLayoutParams(SurfaceActivity.at(0, 0, 96, 96));
                    root.removeView(textureView);
                    addTexture(replaced);
                    return null;
                });
                ScalingProducer.require(replaced.await(30, TimeUnit.SECONDS), "replacement texture unavailable");
                if (local != null) local.replaceTexture(texture);
                else call(ScalingService.REPLACE, 0, texture);
            } finally { oldTexture.release(); }
            render(0);
            phase(4, true, "setOutputSurface keeps mode for next frame");
            render(MediaCodecModes.FIT);
            phase(5, false, "live fit clears prior crop");
            render(MediaCodecModes.CROP);
            if (local != null) local.resetDefaults();
            else call(ScalingService.RESET, 0, null);
            render(0);
            phase(6, false, "reset and reconfigure restore default fit");
            passed = true;
        } catch (Throwable failure) {
            say("FAILED " + failure);
            failure.printStackTrace();
        } finally {
            try {
                if (local != null) { local.close(); local = null; }
                else if (remote != null) call(ScalingService.CLOSE, 0, null);
                if (texture != null) texture.release();
                if (bound) ui(() -> { unbindService(connection); return null; });
            } catch (Throwable failure) {
                passed = false;
                say("FAILED cleanup " + failure);
                failure.printStackTrace();
            }
            if (passed) say("done");
            main.post(this::finish);
        }
    }

    private static final class MediaCodecModes {
        static final int FIT = android.media.MediaCodec.VIDEO_SCALING_MODE_SCALE_TO_FIT;
        static final int CROP = android.media.MediaCodec.VIDEO_SCALING_MODE_SCALE_TO_FIT_WITH_CROPPING;
    }
}
