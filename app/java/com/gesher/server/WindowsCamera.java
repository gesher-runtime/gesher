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
    /** One capture device: its Windows id, name and the frame sizes it offers. */
    public static final class Device {
        public final String id;
        public final String name;
        public final List<Size> sizes;
        public final boolean test;
        private final boolean rear;

        Device(String id, String name, List<Size> sizes, boolean rear) {
            this.id = id;
            this.name = name;
            this.sizes = Collections.unmodifiableList(sizes);
            this.test = "gesher-test".equals(id);
            this.rear = rear;
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

    /** Parses real devices with an explicit rear-mounting choice; enumeration implies no facing. */
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
            found.add(new Device(f[0], f[1], sizes, rear));
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
        public final int width;
        public final int height;

        private Capture(long handle, int width, int height) {
            mHandle = handle;
            this.width = width;
            this.height = height;
        }

        /** The next frame into {@code nv12}; blocks until there is one. 0 or an HRESULT. */
        public int read(byte[] nv12) {
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

        @Override
        public void close() {
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

    private static native void nativeClose(long handle);
}
