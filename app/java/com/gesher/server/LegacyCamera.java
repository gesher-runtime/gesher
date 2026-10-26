/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.ActivityThread;
import android.hardware.Camera;
import android.util.Size;
import android.view.Surface;

import java.lang.reflect.Method;
import java.util.ArrayDeque;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.WeakHashMap;

/**
 * The legacy {@code android.hardware.Camera} over the same devices as Camera2 (hwsvc2 track,
 * ADR 0051): {@link WindowsCamera}'s Media Foundation capture devices (and GESHER_CAMERA_TEST's
 * test camera), in the app's process. compat/devices/camera_frames.cc's natives of
 * android.hardware.Camera call these methods; events go back through Camera's
 * postEventFromNative, as libcameraservice's client does.
 *
 * <p>A device is camera {@code i} in Windows' order (the test camera last); a PC's camera
 * faces the user (CAMERA_FACING_FRONT), the test camera is BACK, orientation 0. Opening needs
 * CAMERA in the app's manifest, else "Fail to connect to camera service". Parameters: the
 * device's sizes for preview, picture and video, preview formats NV21 (yuv420sp, the
 * default) and YV12, 30 fps, fixed focus, no flash, no zoom, no face detection, JPEG
 * pictures and thumbnails. Preview frames go to the preview callback (NV21 or YV12; with
 * callback buffers when the app adds them) and to a preview surface that has a native
 * window ({@code setPreviewDisplay}: frames drawn as RGBA, as Camera2's are);
 * {@code setPreviewTexture}: the same through a Surface over the SurfaceTexture (surfaces
 * track, ADR 0056). {@code takePicture} sends the shutter, a null raw image, a postview if asked and
 * the JPEG at the picture size, then the preview stops, as on Android; autofocus answers
 * success at once (fixed focus).
 */
public final class LegacyCamera {
    private static final int MSG_ERROR = 0x001;
    private static final int MSG_SHUTTER = 0x002;
    private static final int MSG_FOCUS = 0x004;
    private static final int MSG_PREVIEW_FRAME = 0x010;
    private static final int MSG_POSTVIEW_FRAME = 0x040;
    private static final int MSG_RAW_IMAGE = 0x080;
    private static final int MSG_COMPRESSED_IMAGE = 0x100;
    private static final int ENODEV = 19;
    private static final int EACCES = 13;

    private LegacyCamera() {
    }

    private static List<WindowsCamera.Device> devices() {
        return CameraService.get().devices();
    }

    private static void log(String message) {
        CameraService.log("legacy Camera: " + message);
    }

    // --- static: the cameras --------------------------------------------------------------

    public static int count() {
        return devices().size();
    }

    public static void info(int id, Camera.CameraInfo info) {
        List<WindowsCamera.Device> d = devices();
        if (id < 0 || id >= d.size()) throw new RuntimeException("Unknown camera ID");
        info.facing = d.get(id).test ? Camera.CameraInfo.CAMERA_FACING_BACK
                : Camera.CameraInfo.CAMERA_FACING_FRONT;
        info.orientation = 0;
        info.canDisableShutterSound = true;
    }

    // --- one open camera ------------------------------------------------------------------

    private static final class Session {
        final Object weakThis;
        final WindowsCamera.Device device;
        final Map<String, String> params = new LinkedHashMap<>();
        final ArrayDeque<byte[]> buffers = new ArrayDeque<>();
        Surface previewSurface;
        Surface textureSurface;  // setPreviewTexture's (released when replaced)
        boolean callback;
        boolean manualBuffers;
        Thread preview;
        volatile boolean previewing;
        WindowsCamera.Capture capture;
        int audioRestriction;

        Session(Object weakThis, WindowsCamera.Device device) {
            this.weakThis = weakThis;
            this.device = device;
        }
    }

    private static final Map<Camera, Session> sSessions = new WeakHashMap<>();

    private static Session session(Camera camera) {
        synchronized (sSessions) {
            Session s = sSessions.get(camera);
            if (s == null) throw new RuntimeException("Camera is being used after Camera.release() was called");
            return s;
        }
    }

    private static Method sPost;

    /** Camera.postEventFromNative(weakThis, what, arg1, arg2, obj). */
    private static void post(Session s, int what, int arg1, int arg2, Object obj) {
        try {
            Method m = sPost;
            if (m == null) {
                m = Camera.class.getDeclaredMethod("postEventFromNative", Object.class,
                        int.class, int.class, int.class, Object.class);
                m.setAccessible(true);
                sPost = m;
            }
            m.invoke(null, s.weakThis, what, arg1, arg2, obj);
        } catch (ReflectiveOperationException e) {
            log("posting event " + what + " failed: " + e);
        }
    }

    private static String sizes(List<Size> sizes) {
        StringBuilder b = new StringBuilder();
        for (Size z : sizes) {
            if (b.length() > 0) b.append(',');
            b.append(z.getWidth()).append('x').append(z.getHeight());
        }
        return b.toString();
    }

    private static void defaults(Session s) {
        List<Size> sizes = s.device.sizes;
        Size first = sizes.get(0);
        String size = first.getWidth() + "x" + first.getHeight();
        String all = sizes(sizes);
        Map<String, String> p = s.params;
        p.put("preview-size", size);
        p.put("preview-size-values", all);
        p.put("preview-format", "yuv420sp");
        p.put("preview-format-values", "yuv420sp,yuv420p");
        p.put("preview-frame-rate", "30");
        p.put("preview-frame-rate-values", "30");
        p.put("preview-fps-range", "30000,30000");
        p.put("preview-fps-range-values", "(30000,30000)");
        p.put("picture-size", size);
        p.put("picture-size-values", all);
        p.put("picture-format", "jpeg");
        p.put("picture-format-values", "jpeg");
        p.put("jpeg-quality", "90");
        p.put("jpeg-thumbnail-size-values", "160x120,0x0");
        p.put("jpeg-thumbnail-width", "160");
        p.put("jpeg-thumbnail-height", "120");
        p.put("jpeg-thumbnail-quality", "90");
        p.put("video-size", size);
        p.put("video-size-values", all);
        p.put("preferred-preview-size-for-video", size);
        p.put("focus-mode", "fixed");
        p.put("focus-mode-values", "fixed");
        p.put("focal-length", "4.0");
        p.put("focus-distances", "Infinity,Infinity,Infinity");
        p.put("horizontal-view-angle", "60");
        p.put("vertical-view-angle", "45");
        p.put("whitebalance", "auto");
        p.put("whitebalance-values", "auto");
        p.put("antibanding", "auto");
        p.put("antibanding-values", "auto");
        p.put("effect", "none");
        p.put("effect-values", "none");
        p.put("scene-mode", "auto");
        p.put("scene-mode-values", "auto");
        p.put("exposure-compensation", "0");
        p.put("max-exposure-compensation", "0");
        p.put("min-exposure-compensation", "0");
        p.put("exposure-compensation-step", "0");
        p.put("auto-exposure-lock-supported", "false");
        p.put("auto-whitebalance-lock-supported", "false");
        p.put("zoom-supported", "false");
        p.put("smooth-zoom-supported", "false");
        p.put("max-num-focus-areas", "0");
        p.put("max-num-metering-areas", "0");
        p.put("max-num-detected-faces-hw", "0");
        p.put("max-num-detected-faces-sw", "0");
        p.put("video-stabilization-supported", "false");
        p.put("video-snapshot-supported", "false");
        p.put("recording-hint", "false");
    }

    /** native_setup: 0, or -ENODEV / -EACCES (Camera's constructor throws). */
    public static int setup(Camera camera, Object weakThis, int id) {
        List<WindowsCamera.Device> d = devices();
        if (id < 0 || id >= d.size()) return -ENODEV;
        String pkg = ActivityThread.currentPackageName();
        if (!CameraService.hasCameraPermission(pkg)) {
            log(pkg + " does not have android.permission.CAMERA");
            return -EACCES;
        }
        Session s = new Session(weakThis, d.get(id));
        defaults(s);
        synchronized (sSessions) {
            sSessions.put(camera, s);
        }
        log("open " + id + " (" + s.device.name + ") for " + pkg);
        return 0;
    }

    public static void release(Camera camera) {
        Session s;
        synchronized (sSessions) {
            s = sSessions.remove(camera);
        }
        if (s != null) stop(s);
    }

    // --- parameters -----------------------------------------------------------------------

    public static String getParameters(Camera camera) {
        Session s = session(camera);
        StringBuilder b = new StringBuilder();
        synchronized (s) {
            for (Map.Entry<String, String> e : s.params.entrySet()) {
                if (b.length() > 0) b.append(';');
                b.append(e.getKey()).append('=').append(e.getValue());
            }
        }
        return b.toString();
    }

    public static void setParameters(Camera camera, String flattened) {
        Session s = session(camera);
        Map<String, String> p = new LinkedHashMap<>();
        for (String kv : flattened.split(";")) {
            int eq = kv.indexOf('=');
            if (eq > 0) p.put(kv.substring(0, eq), kv.substring(eq + 1));
        }
        String all = sizes(s.device.sizes);
        for (String key : new String[] {"preview-size", "picture-size", "video-size"}) {
            String v = p.get(key);
            if (v != null && !("," + all + ",").contains("," + v + ",")) {
                throw new RuntimeException("setParameters failed");
            }
        }
        String format = p.get("preview-format");
        if (format != null && !format.equals("yuv420sp") && !format.equals("yuv420p")) {
            throw new RuntimeException("setParameters failed");
        }
        synchronized (s) {
            s.params.putAll(p);
        }
    }

    private static Size size(Session s, String key) {
        String v;
        synchronized (s) {
            v = s.params.get(key);
        }
        int x = v != null ? v.indexOf('x') : -1;
        if (x <= 0) return s.device.sizes.get(0);
        return new Size(Integer.parseInt(v.substring(0, x)), Integer.parseInt(v.substring(x + 1)));
    }

    private static int intParam(Session s, String key, int fallback) {
        try {
            synchronized (s) {
                return Integer.parseInt(s.params.get(key));
            }
        } catch (RuntimeException e) {
            return fallback;
        }
    }

    // --- preview ------------------------------------------------------------------------

    public static void setPreviewSurface(Camera camera, Surface surface) {
        Session s = session(camera);
        synchronized (s) {
            releaseTextureSurface(s);
            s.previewSurface = surface;
        }
    }

    // Surfaces track (ADR 0056): a SurfaceTexture's producer Surface takes the frames, as
    // setPreviewDisplay's does (a TextureView shows them).
    public static void setPreviewTexture(Camera camera, Object surfaceTexture) {
        Session s = session(camera);
        synchronized (s) {
            releaseTextureSurface(s);
            if (surfaceTexture instanceof android.graphics.SurfaceTexture) {
                s.textureSurface = new Surface((android.graphics.SurfaceTexture) surfaceTexture);
                s.previewSurface = s.textureSurface;
            } else {
                s.previewSurface = null;
            }
        }
    }

    // With the session's lock held.
    private static void releaseTextureSurface(Session s) {
        if (s.textureSurface != null) {
            if (s.previewSurface == s.textureSurface) s.previewSurface = null;
            s.textureSurface.release();
            s.textureSurface = null;
        }
    }

    public static void setHasPreviewCallback(Camera camera, boolean installed, boolean manual) {
        Session s = session(camera);
        synchronized (s) {
            s.callback = installed;
            s.manualBuffers = manual;
            if (!installed) s.buffers.clear();
        }
    }

    public static void addCallbackBuffer(Camera camera, byte[] buffer, int msgType) {
        Session s = session(camera);
        if (msgType != MSG_PREVIEW_FRAME || buffer == null) return;  // no raw images
        synchronized (s) {
            s.buffers.add(buffer);
        }
    }

    public static boolean previewEnabled(Camera camera) {
        return session(camera).previewing;
    }

    private static WindowsCamera.Capture open(Session s, Size size) {
        try {
            return WindowsCamera.open(s.device.id, size.getWidth(), size.getHeight());
        } catch (WindowsCamera.OpenException e) {
            log(e.getMessage());
            return null;
        }
    }

    public static void startPreview(Camera camera) {
        Session s = session(camera);
        synchronized (s) {
            if (s.previewing) return;
            Size size = size(s, "preview-size");
            s.capture = open(s, size);
            if (s.capture == null) throw new RuntimeException("startPreview failed");
            s.previewing = true;
            WindowsCamera.Capture capture = s.capture;
            s.preview = new Thread(() -> previewLoop(s, capture, size), "legacy-camera-preview");
            s.preview.setDaemon(true);
            s.preview.start();
        }
        log("preview started at " + size(s, "preview-size"));
    }

    private static void previewLoop(Session s, WindowsCamera.Capture capture, Size size) {
        byte[] nv12 = new byte[capture.width * capture.height * 3 / 2];
        boolean surfaceFailed = false;
        while (s.previewing) {
            int hr = capture.read(nv12);
            if (!s.previewing) break;
            if (hr != 0) {
                log(String.format("reading a frame: 0x%08x", hr));
                post(s, MSG_ERROR, 1 /* CAMERA_ERROR_UNKNOWN */, 0, null);
                break;
            }
            int w = size.getWidth();
            int h = size.getHeight();
            Surface surface;
            boolean callback;
            boolean manual;
            synchronized (s) {
                surface = s.previewSurface;
                callback = s.callback;
                manual = s.manualBuffers;
            }
            if (surface != null && !surfaceFailed) {
                int r = CameraFrames.deliver(surface, nv12, capture.width, capture.height,
                        System.nanoTime(), null);
                if (r != CameraFrames.OK) {
                    surfaceFailed = true;
                    log("the preview surface takes no frames (no native window)");
                }
            }
            if (callback) {
                byte[] frame = frame(s, nv12, capture.width, capture.height, w, h);
                if (manual) {
                    byte[] buffer;
                    synchronized (s) {
                        buffer = s.buffers.poll();
                    }
                    if (buffer == null || buffer.length < frame.length) continue;  // dropped
                    System.arraycopy(frame, 0, buffer, 0, frame.length);
                    frame = buffer;
                }
                post(s, MSG_PREVIEW_FRAME, 0, 0, frame);
            }
        }
    }

    /** A preview frame in the preview format: NV21 (yuv420sp) or YV12 (yuv420p). */
    private static byte[] frame(Session s, byte[] nv12, int cw, int ch, int w, int h) {
        byte[] nv21 = CameraFrames.nv21(nv12, cw, ch, w, h);
        String format;
        synchronized (s) {
            format = s.params.get("preview-format");
        }
        if (!"yuv420p".equals(format)) return nv21;
        // YV12: Y (stride aligned to 16), then V, then U (strides aligned to 16).
        int yStride = (w + 15) / 16 * 16;
        int cStride = (yStride / 2 + 15) / 16 * 16;
        int ch2 = h / 2;
        byte[] out = new byte[yStride * h + 2 * cStride * ch2];
        for (int y = 0; y < h; y++) System.arraycopy(nv21, y * w, out, y * yStride, w);
        int vAt = yStride * h;
        int uAt = vAt + cStride * ch2;
        for (int y = 0; y < ch2; y++) {
            for (int x = 0; x < w / 2; x++) {
                int at = w * h + y * w + x * 2;
                out[vAt + y * cStride + x] = nv21[at];
                out[uAt + y * cStride + x] = nv21[at + 1];
            }
        }
        return out;
    }

    public static void stopPreview(Camera camera) {
        stop(session(camera));
    }

    private static void stop(Session s) {
        Thread t;
        WindowsCamera.Capture capture;
        synchronized (s) {
            if (!s.previewing && s.capture == null) return;
            s.previewing = false;
            t = s.preview;
            capture = s.capture;
            s.preview = null;
            s.capture = null;
        }
        // The preview thread ends after the frame it is reading (a closed capture must not
        // be read from), then the capture closes.
        if (t != null && t != Thread.currentThread()) {
            try {
                t.join(3000);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
        }
        if (capture != null) capture.close();
        log("preview stopped");
    }

    // --- pictures -------------------------------------------------------------------------

    public static void autoFocus(Camera camera) {
        post(session(camera), MSG_FOCUS, 1 /* success */, 0, null);
    }

    public static void takePicture(Camera camera, int msgType) {
        Session s = session(camera);
        if (!s.previewing) throw new RuntimeException("takePicture failed");
        Size picture = size(s, "picture-size");
        int quality = intParam(s, "jpeg-quality", 90);
        Thread t = new Thread(() -> {
            // The frame comes from a capture at the picture size; the preview stops, as
            // a picture ends it on Android.
            stop(s);
            WindowsCamera.Capture c = open(s, picture);
            if (c == null) {
                post(s, MSG_ERROR, 1, 0, null);
                return;
            }
            byte[] nv12 = new byte[c.width * c.height * 3 / 2];
            int hr;
            try {
                hr = c.read(nv12);
            } finally {
                c.close();
            }
            if (hr != 0) {
                post(s, MSG_ERROR, 1, 0, null);
                return;
            }
            if ((msgType & MSG_SHUTTER) != 0) post(s, MSG_SHUTTER, 0, 0, null);
            if ((msgType & MSG_RAW_IMAGE) != 0) post(s, MSG_RAW_IMAGE, 0, 0, null);
            if ((msgType & MSG_POSTVIEW_FRAME) != 0) {
                post(s, MSG_POSTVIEW_FRAME, 0, 0,
                        CameraFrames.nv21(nv12, c.width, c.height, c.width, c.height));
            }
            if ((msgType & MSG_COMPRESSED_IMAGE) != 0) {
                byte[] jpeg = CameraFrames.jpeg(nv12, c.width, c.height, picture.getWidth(),
                        picture.getHeight(), quality);
                log("picture taken: " + picture + ", JPEG " + jpeg.length + " bytes");
                post(s, MSG_COMPRESSED_IMAGE, 0, 0, jpeg);
            }
        }, "legacy-camera-picture");
        t.setDaemon(true);
        t.start();
    }

    // --- what a fixed-focus camera without zoom or face detection answers --------------------

    public static void startSmoothZoom(Camera camera, int value) {
        session(camera);
        throw new RuntimeException("start smooth zoom failed");
    }

    public static void startFaceDetection(Camera camera, int type) {
        session(camera);
        throw new RuntimeException("start face detection failed");
    }

    public static void setAudioRestriction(Camera camera, int mode) {
        Session s = session(camera);
        synchronized (s) {
            s.audioRestriction = mode;
        }
    }

    public static int getAudioRestriction(Camera camera) {
        Session s = session(camera);
        synchronized (s) {
            return s.audioRestriction;
        }
    }

    /** Whether {@code camera} is open (the natives' check for the calls that do nothing). */
    public static void check(Camera camera) {
        session(camera);
    }

}
