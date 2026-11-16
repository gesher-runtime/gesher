/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.util.Size;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
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

    /** Optional file-backed image provider loaded reflectively at runtime. */
    public interface ImageFrameProvider {
        boolean hasActiveImage();
        boolean readNv12(byte[] nv12, int width, int height);
    }

    private static List<Device> sDevices;
    private static volatile ImageFrameProvider sImageProvider;
    private static volatile boolean sImageProviderChecked;

    private WindowsCamera() {
    }

    static void setImageProvider(ImageFrameProvider provider) {
        sImageProvider = provider;
        sImageProviderChecked = true;
    }

    private static ImageFrameProvider imageProvider() {
        if (!sImageProviderChecked) {
            synchronized (WindowsCamera.class) {
                if (!sImageProviderChecked) {
                    try {
                        Class<?> cls = Class.forName("com.gesher.server.CameraImageSource");
                        sImageProvider = (ImageFrameProvider) cls.getDeclaredConstructor().newInstance();
                    } catch (Throwable ignored) {
                    }
                    sImageProviderChecked = true;
                }
            }
        }
        return sImageProvider;
    }

    /** The devices (found once per process). Empty without Gesher's natives. */
    public static synchronized List<Device> devices() {
        List<Device> devices = sDevices;
        if (devices != null) return devices;
        devices = new ArrayList<>();
        String rearSelection = System.getenv("GESHER_CAMERA_REAR_ID");
        boolean hasSelection = rearSelection != null && !rearSelection.isEmpty();
        boolean testCameraEnabled = false;
        String testEnv = System.getenv("GESHER_CAMERA_TEST");
        if (testEnv != null && !testEnv.isEmpty() && !"0".equals(testEnv)) {
            testCameraEnabled = true;
        }
        try {
            boolean assignedDefaultRear = false;
            for (String line : nativeList()) {
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
                boolean rear = false;
                if (!isTest) {
                    if (hasSelection) {
                        rear = rearSelection.equals(selectionKey(f[0]));
                    } else if (!testCameraEnabled && !assignedDefaultRear) {
                        rear = true;
                        assignedDefaultRear = true;
                    }
                }
                devices.add(new Device(f[0], f[1], sizes, rear));
            }
        } catch (UnsatisfiedLinkError e) {
            // Not Gesher's runtime: no cameras.
        }
        if (devices.isEmpty() && !testCameraEnabled) {
            ImageFrameProvider provider = imageProvider();
            if (provider != null && provider.hasActiveImage()) {
                devices.add(new Device("gesher-image", "Virtual Image Camera",
                        Arrays.asList(new Size(1280, 720), new Size(1920, 1080)), true));
            }
        }
        sDevices = List.copyOf(devices);
        return sDevices;
    }

    private static boolean isPhysicalRear(String id) {
        List<Device> devices = sDevices;
        if (devices == null) {
            devices = devices();
        }
        for (Device d : devices) {
            if (d.id.equals(id)) {
                return !d.test && d.isRearFacing();
            }
        }
        return false;
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
        private final boolean mAllowImageOverride;
        private final boolean mVirtualImage;
        public final int width;
        public final int height;

        private Capture(long handle, int width, int height) {
            this(handle, width, height, false, false);
        }

        private Capture(long handle, int width, int height, boolean allowImageOverride, boolean virtualImage) {
            mHandle = handle;
            this.width = width;
            this.height = height;
            mAllowImageOverride = allowImageOverride;
            mVirtualImage = virtualImage;
        }

        /** The next frame into {@code nv12}; blocks until there is one. 0 or an HRESULT. */
        public int read(byte[] nv12) {
            if (mAllowImageOverride) {
                synchronized (this) {
                    if (mHandle == 0) return 0x80004004;  // E_ABORT
                }
                ImageFrameProvider provider = imageProvider();
                boolean served = provider != null && provider.readNv12(nv12, width, height);
                if (!served && mVirtualImage) {
                    fillNeutralNv12(nv12, width, height);
                    served = true;
                }
                if (served) {
                    synchronized (this) {
                        if (mHandle == 0) return 0x80004004;
                        try {
                            wait(50);
                        } catch (InterruptedException e) {
                            Thread.currentThread().interrupt();
                        }
                        if (mHandle == 0) return 0x80004004;
                    }
                    return 0;
                }
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

        private static void fillNeutralNv12(byte[] nv12, int width, int height) {
            int ySize = width * height;
            int total = ySize * 3 / 2;
            if (nv12 == null || nv12.length < total) return;
            Arrays.fill(nv12, 0, ySize, (byte) 24);
            Arrays.fill(nv12, ySize, total, (byte) 128);
        }

        @Override
        public void close() {
            long handle;
            synchronized (this) {
                if (mHandle == 0) return;
                handle = mVirtualImage ? 0 : mHandle;
                mHandle = 0;
                notifyAll();
                // ReadSample may block. Close rejects new reads immediately,
                // while the last admitted reader releases the native pointer.
                if (handle != 0 && mReaders != 0) {
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
        if ("gesher-image".equals(id)) {
            int w = width > 0 ? width : 1280;
            int h = height > 0 ? height : 720;
            return new Capture(-1L, w, h, true, true);
        }
        int[] out = new int[3];
        long handle;
        try {
            handle = nativeOpen(id, width, height, out);
        } catch (UnsatisfiedLinkError e) {
            throw new OpenException(id, 0x80004001);
        }
        if (handle == 0) throw new OpenException(id, out[2]);
        return new Capture(handle, out[0], out[1], isPhysicalRear(id), false);
    }

    private static native String[] nativeList();

    private static native String nativeStatus();

    private static native long nativeOpen(String id, int width, int height, int[] out);

    private static native int nativeRead(long handle, byte[] nv12);

    private static native void nativeClose(long handle);
}
