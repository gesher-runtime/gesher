/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.surfaceapp;

import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.SurfaceTexture;
import android.hardware.Camera;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureRequest;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.util.Log;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.TextureView;
import android.view.View;
import android.widget.FrameLayout;

import java.util.Arrays;

/**
 * The surfaces track's test Activity (tests/windows/tracks/52-surfaces.ps1). Its window is
 * 400x800 at 160 dpi (1 dp = 1 px), white, with views placed by FrameLayout margins:
 *
 * <pre>
 * phase 1  SurfaceView A (0,0)-(200,200), below the window (the default), drawn red with
 *          lockCanvas; a green View (150,150)-(250,250) over it.
 *          SurfaceView B (0,250)-(200,450), setZOrderOnTop, drawn blue; a yellow View
 *          (150,400)-(250,500) under it although later in the view tree.
 *          TextureView T (250,250)-(400,400), drawn cyan through a Surface over its
 *          SurfaceTexture; TextureView.getBitmap.
 * phase 2  A.setFixedSize(50, 25): its buffer, left half red and right half green, scaled
 *          to the view.
 * phase 3  A back to its layout's size and moved/resized to (50,550)-(350,650): red there,
 *          white where it was.
 * phase 4  A hidden (surfaceDestroyed) and shown again (surfaceCreated), drawn red.
 * phase 5  (a camera) the legacy Camera's preview into SurfaceView C (0,0)-(320,240)
 *          (setPreviewDisplay); A, B and T hidden.
 * phase 6  the legacy Camera's preview into TextureView U (0,300)-(320,540)
 *          (setPreviewTexture).
 * phase 7  Camera2: one repeating request into C's and U's surfaces.
 * </pre>
 *
 * Each phase logs "surfaces: phase N ready" once drawn and holds for {@link #HOLD_MS}, so
 * the test reads the window back; then "surfaces: done" and the Activity finishes. Checks
 * without the window (Bitmap parcels, YuvImage, SurfaceTexture) run first ({@link Checks}).
 */
public class SurfaceActivity extends Activity {
    static final String TAG = "surfaces";
    static final long HOLD_MS = 6000;

    static void say(String message) {
        System.out.println("surfaces: " + message);
        System.out.flush();
        Log.i(TAG, message);
    }

    private final Handler mMain = new Handler(Looper.getMainLooper());
    private FrameLayout mRoot;
    private SurfaceView mA, mB, mC;
    private TextureView mT, mU;
    private boolean mFixed;
    private boolean mPhase1Announced;

    static FrameLayout.LayoutParams at(int x, int y, int w, int h) {
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(w, h);
        lp.leftMargin = x;
        lp.topMargin = y;
        return lp;
    }

    static View box(Activity a, int color) {
        View v = new View(a);
        v.setBackgroundColor(color);
        return v;
    }

    /** Fills the holder's surface: one color, or two halves (left, right). */
    static void fill(SurfaceHolder holder, int left, int right, String what) {
        Canvas c = holder.lockCanvas();
        if (c == null) {
            say(what + ": lockCanvas answered null");
            return;
        }
        int w = c.getWidth(), h = c.getHeight();
        Paint p = new Paint();
        p.setColor(left);
        c.drawRect(0, 0, left == right ? w : w / 2f, h, p);
        if (left != right) {
            p.setColor(right);
            c.drawRect(w / 2f, 0, w, h, p);
        }
        holder.unlockCanvasAndPost(c);
        say(what + ": drew " + w + "x" + h);
    }

    class Callbacks implements SurfaceHolder.Callback {
        final String mName;
        final int mColor;

        Callbacks(String name, int color) {
            mName = name;
            mColor = color;
        }

        @Override
        public void surfaceCreated(SurfaceHolder holder) {
            say(mName + " surfaceCreated, valid " + holder.getSurface().isValid());
        }

        @Override
        public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
            say(mName + " surfaceChanged format " + format + " " + width + "x" + height
                    + ", frame " + holder.getSurfaceFrame().width() + "x"
                    + holder.getSurfaceFrame().height());
            if (mColor == 0) return;  // a camera's
            if (mName.equals("A") && mFixed) {
                fill(holder, Color.RED, Color.GREEN, mName);
            } else {
                fill(holder, mColor, mColor, mName);
            }
            if (mName.equals("B")) mMain.postDelayed(SurfaceActivity.this::phase1Ready, 1500);
        }

        @Override
        public void surfaceDestroyed(SurfaceHolder holder) {
            say(mName + " surfaceDestroyed");
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Checks.run(this);

        mRoot = new FrameLayout(this);
        mRoot.setBackgroundColor(Color.WHITE);
        mA = new SurfaceView(this);
        mA.getHolder().addCallback(new Callbacks("A", Color.RED));
        mRoot.addView(mA, at(0, 0, 200, 200));
        mRoot.addView(box(this, Color.GREEN), at(150, 150, 100, 100));
        mB = new SurfaceView(this);
        mB.setZOrderOnTop(true);
        mB.getHolder().addCallback(new Callbacks("B", Color.BLUE));
        mRoot.addView(mB, at(0, 250, 200, 200));
        mRoot.addView(box(this, Color.YELLOW), at(150, 400, 100, 100));
        mT = new TextureView(this);
        mT.setSurfaceTextureListener(new TextureView.SurfaceTextureListener() {
            @Override
            public void onSurfaceTextureAvailable(SurfaceTexture st, int width, int height) {
                say("T onSurfaceTextureAvailable " + width + "x" + height);
                Surface s = new Surface(st);
                Canvas c = s.lockCanvas(null);
                c.drawColor(Color.CYAN);
                s.unlockCanvasAndPost(c);
                s.release();
                say("T: drew cyan");
            }

            @Override
            public void onSurfaceTextureSizeChanged(SurfaceTexture st, int width, int height) {
                say("T onSurfaceTextureSizeChanged " + width + "x" + height);
            }

            @Override
            public boolean onSurfaceTextureDestroyed(SurfaceTexture st) {
                say("T onSurfaceTextureDestroyed");
                return true;
            }

            @Override
            public void onSurfaceTextureUpdated(SurfaceTexture st) {
            }
        });
        mRoot.addView(mT, at(250, 250, 150, 150));
        setContentView(mRoot);
        // Phase 1 is announced when B has drawn (Callbacks), or after 20 s regardless.
        mMain.postDelayed(this::phase1Ready, 20000);
    }

    private void phase1Ready() {
        if (mPhase1Announced) return;
        mPhase1Announced = true;
        Bitmap b = mT.getBitmap();
        say("T getBitmap " + (b == null ? "null"
                : b.getWidth() + "x" + b.getHeight() + " center "
                        + String.format("%08x", b.getPixel(b.getWidth() / 2, b.getHeight() / 2))));
        say("phase 1 ready");
        mMain.postDelayed(this::phase2, HOLD_MS);
    }

    private void phase2() {
        mFixed = true;
        mA.getHolder().setFixedSize(50, 25);
        // TextureView.lockCanvas: a magenta frame into T's SurfaceTexture.
        Canvas c = mT.lockCanvas();
        if (c == null) {
            say("T lockCanvas answered null");
        } else {
            String size = c.getWidth() + "x" + c.getHeight();
            c.drawColor(Color.MAGENTA);
            mT.unlockCanvasAndPost(c);
            say("T lockCanvas: drew magenta " + size);
        }
        mMain.postDelayed(() -> {
            say("phase 2 ready");
            mMain.postDelayed(this::phase3, HOLD_MS);
        }, 2000);
    }

    private void phase3() {
        mFixed = false;
        mA.getHolder().setSizeFromLayout();
        mA.setLayoutParams(at(50, 550, 300, 100));
        mMain.postDelayed(() -> {
            say("phase 3 ready");
            mMain.postDelayed(this::phase4, HOLD_MS);
        }, 2000);
    }

    private void phase4() {
        mA.setVisibility(View.GONE);
        mMain.postDelayed(() -> {
            mA.setVisibility(View.VISIBLE);
            mMain.postDelayed(() -> {
                say("phase 4 ready");
                mMain.postDelayed(this::minimize, HOLD_MS);
            }, 2000);
        }, 1500);
    }

    // The test minimizes the window (the Activity stops, the surfaces go) and restores it
    // (they come back); phase 5 follows the restore, or 40 s regardless.
    private boolean mAwaitRestore;
    private boolean mStoppedOnce;

    private void minimize() {
        mAwaitRestore = true;
        say("minimize now");
        mMain.postDelayed(() -> {
            if (mAwaitRestore) {
                mAwaitRestore = false;
                say("no restore within 40 s");
                phase5();
            }
        }, 40000);
    }

    @Override
    protected void onStop() {
        super.onStop();
        say("onStop");
        if (mAwaitRestore) mStoppedOnce = true;
    }

    // --- cameras -------------------------------------------------------------------------

    private HandlerThread mCameraThread;
    private Handler mCameraHandler;
    @SuppressWarnings("deprecation")
    private Camera mLegacy;

    @SuppressWarnings("deprecation")
    private void phase5() {
        if (Camera.getNumberOfCameras() == 0) {
            say("no camera: phases 5 to 7 skipped");
            finishRun();
            return;
        }
        mA.setVisibility(View.GONE);
        mB.setVisibility(View.GONE);
        mT.setVisibility(View.GONE);
        mC = new SurfaceView(this);
        mC.getHolder().addCallback(new Callbacks("C", 0) {
            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                super.surfaceChanged(holder, format, width, height);
                if (mLegacy != null) return;
                mCameraThread = new HandlerThread("camera");
                mCameraThread.start();
                mCameraHandler = new Handler(mCameraThread.getLooper());
                mCameraHandler.post(() -> {
                    try {
                        mLegacy = Camera.open(Camera.getNumberOfCameras() - 1);
                        Camera.Parameters p = mLegacy.getParameters();
                        p.setPreviewSize(320, 240);
                        mLegacy.setParameters(p);
                        mLegacy.setPreviewDisplay(holder);
                        mLegacy.startPreview();
                        say("legacy camera: preview into SurfaceView C started");
                    } catch (Exception e) {
                        say("legacy camera into C failed: " + e);
                    }
                    mMain.postDelayed(() -> {
                        say("phase 5 ready");
                        mMain.postDelayed(SurfaceActivity.this::phase6, HOLD_MS);
                    }, 3000);
                });
            }
        });
        mRoot.addView(mC, at(0, 0, 320, 240));
        mU = new TextureView(this);
        mRoot.addView(mU, at(0, 300, 320, 240));
    }

    @SuppressWarnings("deprecation")
    private void phase6() {
        mCameraHandler.post(() -> {
            try {
                mLegacy.stopPreview();
                mLegacy.setPreviewDisplay(null);
                SurfaceTexture st = mU.getSurfaceTexture();
                if (st == null) {
                    say("legacy camera: TextureView U has no SurfaceTexture");
                } else {
                    mLegacy.setPreviewTexture(st);
                    mLegacy.startPreview();
                    say("legacy camera: preview into TextureView U started");
                }
            } catch (Exception e) {
                say("legacy camera into U failed: " + e);
            }
            mMain.postDelayed(() -> {
                Bitmap b = mU.getBitmap();
                say("U getBitmap " + (b == null ? "null" : b.getWidth() + "x" + b.getHeight()
                        + " center " + String.format("%08x", b.getPixel(160, 120))));
                say("phase 6 ready");
                mMain.postDelayed(this::phase7, HOLD_MS);
            }, 3000);
        });
    }

    @SuppressWarnings("deprecation")
    private void phase7() {
        mCameraHandler.post(() -> {
            mLegacy.stopPreview();
            mLegacy.release();
            mLegacy = null;
            say("legacy camera released");
            try {
                CameraManager cm = getSystemService(CameraManager.class);
                String id = cm.getCameraIdList()[cm.getCameraIdList().length - 1];
                cm.openCamera(id, new CameraDevice.StateCallback() {
                    @Override
                    public void onOpened(CameraDevice device) {
                        mCameraDevice = device;
                        startCamera2(device);
                    }

                    @Override
                    public void onDisconnected(CameraDevice device) {
                        device.close();
                    }

                    @Override
                    public void onError(CameraDevice device, int error) {
                        say("Camera2 error " + error);
                    }
                }, mCameraHandler);
            } catch (Exception e) {
                say("Camera2 open failed: " + e);
                finishRun();
            }
        });
    }

    private CameraDevice mCameraDevice;

    @SuppressWarnings("deprecation")
    private void startCamera2(CameraDevice device) {
        try {
            SurfaceTexture st = mU.getSurfaceTexture();
            st.setDefaultBufferSize(320, 240);
            Surface u = new Surface(st);
            Surface c = mC.getHolder().getSurface();
            device.createCaptureSession(Arrays.asList(c, u), new CameraCaptureSession.StateCallback() {
                @Override
                public void onConfigured(CameraCaptureSession session) {
                    try {
                        CaptureRequest.Builder b =
                                device.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
                        b.addTarget(c);
                        b.addTarget(u);
                        session.setRepeatingRequest(b.build(), null, mCameraHandler);
                        say("Camera2: repeating preview into C and U");
                    } catch (Exception e) {
                        say("Camera2 request failed: " + e);
                    }
                    mMain.postDelayed(() -> {
                        Bitmap bm = mU.getBitmap();
                        say("U getBitmap (Camera2) " + (bm == null ? "null"
                                : String.format("%08x", bm.getPixel(160, 120))));
                        say("phase 7 ready");
                        mMain.postDelayed(() -> {
                            session.close();
                            device.close();
                            finishRun();
                        }, HOLD_MS);
                    }, 3000);
                }

                @Override
                public void onConfigureFailed(CameraCaptureSession session) {
                    say("Camera2 session failed");
                    finishRun();
                }
            }, mCameraHandler);
        } catch (Exception e) {
            say("Camera2 session failed: " + e);
            finishRun();
        }
    }

    private void finishRun() {
        mMain.post(() -> {
            say("done");
            if (mCameraThread != null) mCameraThread.quitSafely();
            finish();
        });
    }

    @Override
    protected void onPause() {
        super.onPause();
        say("onPause");
    }

    @Override
    protected void onResume() {
        super.onResume();
        say("onResume");
        if (mAwaitRestore && mStoppedOnce) {
            mAwaitRestore = false;
            mMain.postDelayed(this::phase5, 3000);
        }
    }
}
