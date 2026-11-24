/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.util.Size;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Base64;
import java.util.Collections;
import java.util.List;

/**
 * The PC's cameras (Media Foundation's video capture devices, compat/devices/camera_mf.cc)
 * and, with GESHER_CAMERA_TEST set, a test camera of known frames: the devices behind
 * {@link CameraService} (docs/devices.md). Frames are NV12.
 */
public final class WindowsCamera {
    /**
     * One capture device: its Windows id, name, the frame sizes it offers and whether its
     * focus really runs automatically ({@link #autofocus}).
     */
    public static final class Device {
        public final String id;
        public final String name;
        public final List<Size> sizes;
        public final boolean test;
        /**
         * The driver's focus control offers automatic focus and a manual lens position
         * (compat/devices/camera_mf.cc's HasAutofocus), so Gesher can run, observe and lock
         * it. False for a fixed-focus camera and always for the test camera.
         */
        public final boolean autofocus;
        private final boolean rear;

        Device(String id, String name, List<Size> sizes, boolean rear) {
            this(id, name, sizes, rear, false);
        }

        Device(String id, String name, List<Size> sizes, boolean rear, boolean autofocus) {
            this.id = id;
            this.name = name;
            this.sizes = Collections.unmodifiableList(sizes);
            this.test = "gesher-test".equals(id);
            this.rear = rear;
            this.autofocus = autofocus && !test;
        }

        public boolean isRearFacing() {
            return test || rear;
        }
    }

    private static volatile List<Device> sDevices;

    private WindowsCamera() {
    }

    /** The devices (found once per process). Empty without Gesher's natives. */
    public static List<Device> devices() {
        List<Device> devices = sDevices;
        if (devices != null) return devices;
        List<Device> found = new ArrayList<>();
        String rearSelection = System.getenv("GESHER_CAMERA_REAR_ID");
        boolean hasSelection = rearSelection != null && !rearSelection.isEmpty();
        String testEnv = System.getenv("GESHER_CAMERA_TEST");
        boolean testCameraEnabled = testEnv != null && !testEnv.isEmpty() && !"0".equals(testEnv);
        try {
            found = resolveDevices(nativeList(), rearSelection, hasSelection, testCameraEnabled);
        } catch (UnsatisfiedLinkError e) {
            // Not Gesher's runtime: no cameras.
        }
        devices = Collections.unmodifiableList(found);
        sDevices = devices;
        return devices;
    }

    /**
     * Parses real devices with an explicit rear-mounting choice; enumeration implies no facing.
     * A line is the id, the name, {@code WxH,WxH,...} and the flags, separated by tabs
     * (devices_jni.cc); the flag {@code af}
     * marks real autofocus. A line without flags (older natives) has fixed focus, and the
     * test camera has fixed focus whatever its line says.
     */
    static List<Device> resolveDevices(String[] lines, String rearSelection,
            boolean hasSelection, boolean testCameraEnabled) {
        List<Device> found = new ArrayList<>();
        if (lines == null) return found;
        for (String line : lines) {
            String[] f = line.split("\t", -1);
            if (f.length < 3) continue;
            List<Size> sizes = new ArrayList<>();
            for (String s : f[2].split(",")) {
                int x = s.indexOf('x');
                if (x > 0) {
                    sizes.add(new Size(Integer.parseInt(s.substring(0, x)),
                            Integer.parseInt(s.substring(x + 1))));
                }
            }
            if (sizes.isEmpty()) continue;
            boolean isTest = "gesher-test".equals(f[0]);
            if (isTest && !testCameraEnabled) continue;
            boolean rear = false;
            if (!isTest) {
                if (hasSelection) {
                    rear = rearSelection.equals(selectionKey(f[0]));
                }
            }
            boolean autofocus = false;
            if (!isTest && f.length > 3) {
                for (String flag : f[3].split(",")) {
                    if ("af".equals(flag)) autofocus = true;
                }
            }
            found.add(new Device(f[0], f[1], sizes, rear, autofocus));
        }
        return found;
    }

    static String selectionKey(String windowsId) {
        // The symbolic link survives enumeration order changes. Encode it for the
        // runtime's narrow Windows environment, including any Unicode characters.
        return Base64.getUrlEncoder().withoutPadding().encodeToString(
                windowsId.getBytes(StandardCharsets.UTF_8));
    }

    /** What Media Foundation answered, for the logs. */
    public static String status() {
        try {
            return nativeStatus();
        } catch (UnsatisfiedLinkError e) {
            return "no camera natives";
        }
    }

    /** An open device: NV12 frames of {@link #width} x {@link #height}. */
    public static final class Capture implements AutoCloseable {
        private long mHandle;
        private long mRetiredHandle;
        private int mReaders;
        /** A selected image's frame (CameraImageSource), repeated instead of a device's. */
        private final byte[] mFrame;
        private volatile boolean mFrameClosed;
        public final int width;
        public final int height;

        private Capture(long handle, int width, int height) {
            mHandle = handle;
            mFrame = null;
            this.width = width;
            this.height = height;
        }

        private Capture(byte[] frame, int width, int height) {
            mFrame = frame;
            this.width = width;
            this.height = height;
        }

        /**
         * A capture whose every frame is {@code nv12} ({@code width} x {@code height}), paced
         * at 30 frames a second: the image the user selected instead of a camera's view
         * (CameraImageSource, the Control Center's image-picker setting). No focus control.
         */
        static Capture ofFrame(byte[] nv12, int width, int height) {
            if (width <= 0 || height <= 0 || nv12.length < width * height * 3 / 2) {
                throw new IllegalArgumentException("NV12 frame does not match " + width + "x" + height);
            }
            return new Capture(nv12, width, height);
        }

        /** The next frame into {@code nv12}; blocks until there is one. 0 or an HRESULT. */
        public int read(byte[] nv12) {
            if (mFrame != null) {
                if (mFrameClosed) return 0x80004004;  // E_ABORT
                try {
                    Thread.sleep(33);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    return 0x80004004;
                }
                if (mFrameClosed) return 0x80004004;
                System.arraycopy(mFrame, 0, nv12, 0, Math.min(nv12.length, mFrame.length));
                return 0;
            }
            long handle;
            synchronized (this) {
                if (mHandle == 0) return 0x80004004;  // E_ABORT
                handle = mHandle;
                ++mReaders;
            }
            try {
                return nativeRead(handle, nv12);
            } finally {
                long retired = 0;
                synchronized (this) {
                    if (--mReaders == 0) {
                        retired = mRetiredHandle;
                        mRetiredHandle = 0;
                    }
                }
                if (retired != 0) nativeClose(retired);
            }
        }

        /** {@link #focus}'s operations (compat/devices/camera_mf.h's FocusOp). */
        public static final int FOCUS_GET = 0, FOCUS_AUTO = 1, FOCUS_LOCK = 2;

        /**
         * Controls the device's focus: {@link #FOCUS_GET} reads the lens position,
         * {@link #FOCUS_AUTO} turns the driver's automatic focus on, {@link #FOCUS_LOCK}
         * turns it off with the lens kept where it is. {@code out} (two values, may be null)
         * receives the lens position and the driver's flags (1 auto, 2 manual) afterwards.
         * 0 or an HRESULT (E_NOTIMPL without a focus control, as for the test camera).
         */
        public int focus(int op, int[] out) {
            if (mFrame != null) return mFrameClosed ? 0x80004004 : 0x80004001;  // E_NOTIMPL
            long handle;
            synchronized (this) {
                if (mHandle == 0) return 0x80004004;  // E_ABORT
                handle = mHandle;
                ++mReaders;
            }
            try {
                return nativeFocus(handle, op, out);
            } catch (UnsatisfiedLinkError e) {
                return 0x80004001;  // E_NOTIMPL
            } finally {
                long retired = 0;
                synchronized (this) {
                    if (--mReaders == 0) {
                        retired = mRetiredHandle;
                        mRetiredHandle = 0;
                    }
                }
                if (retired != 0) nativeClose(retired);
            }
        }

        @Override
        public void close() {
            if (mFrame != null) {
                mFrameClosed = true;
                return;
            }
            long handle;
            synchronized (this) {
                if (mHandle == 0) return;
                handle = mHandle;
                mHandle = 0;
                // ReadSample may block. Close rejects new reads immediately,
                // while the last admitted reader releases the native pointer.
                if (mReaders != 0) {
                    mRetiredHandle = handle;
                    handle = 0;
                }
            }
            if (handle != 0) nativeClose(handle);
        }
    }

    /** A failure to open a device, with its HRESULT. */
    public static final class OpenException extends Exception {
        public final int hr;

        OpenException(String id, int hr) {
            super("opening camera " + id + ": " + String.format("HRESULT 0x%08X", hr));
            this.hr = hr;
        }
    }

    /** Opens {@code id} at its offered size nearest to {@code width} x {@code height}. */
    public static Capture open(String id, int width, int height) throws OpenException {
        int[] out = new int[3];
        long handle;
        try {
            handle = nativeOpen(id, width, height, out);
        } catch (UnsatisfiedLinkError e) {
            throw new OpenException(id, 0x80004001);
        }
        if (handle == 0) throw new OpenException(id, out[2]);
        return new Capture(handle, out[0], out[1]);
    }

    private static native String[] nativeList();

    private static native String nativeStatus();

    private static native long nativeOpen(String id, int width, int height, int[] out);

    private static native int nativeRead(long handle, byte[] nv12);

    private static native int nativeFocus(long handle, int op, int[] out);

    private static native void nativeClose(long handle);
}
