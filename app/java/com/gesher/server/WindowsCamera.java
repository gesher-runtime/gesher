/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.util.Size;

import java.io.File;
import java.io.IOException;
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

    public static final String IMAGE_PICKER_ID = "gesher-image-picker";

    interface ImageSource {
        String[] pick() throws IOException;
    }

    static volatile ImageSource sImageSource;
    private static volatile List<Device> sPhysicalDevices;

    private WindowsCamera() {
    }

    /** Physical cameras (plus GESHER_CAMERA_TEST if enabled), cached once per process. */
    static List<Device> physicalDevices() {
        List<Device> devices = sPhysicalDevices;
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
        sPhysicalDevices = devices;
        return devices;
    }

    /**
     * The devices (physical cameras, plus the virtual image picker when
     * {@link CameraImageFallback#enabled()} is true). Empty without Gesher's natives and
     * without the image-picker fallback.
     */
    public static List<Device> devices() {
        List<Device> physical = physicalDevices();
        if (!CameraImageFallback.enabled()) return physical;
        for (Device d : physical) {
            if (IMAGE_PICKER_ID.equals(d.id)) return physical;
        }
        List<Device> withPicker = new ArrayList<>(physical.size() + 1);
        withPicker.addAll(physical);
        withPicker.add(new Device(IMAGE_PICKER_ID, "Gesher Image Picker",
                List.of(new Size(1280, 720), new Size(640, 480)), true, false));
        return Collections.unmodifiableList(withPicker);
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
        private final byte[] mStaticNv12;
        private volatile boolean mStaticClosed;
        public final File imageFile;
        public final int width;
        public final int height;

        private Capture(long handle, int width, int height) {
            mHandle = handle;
            mStaticNv12 = null;
            this.imageFile = null;
            this.width = width;
            this.height = height;
        }

        private Capture(byte[] staticNv12, File imageFile, int width, int height) {
            mHandle = 0;
            mStaticNv12 = staticNv12;
            this.imageFile = imageFile;
            this.width = width;
            this.height = height;
        }

        /** The next frame into {@code nv12}; blocks until there is one. 0 or an HRESULT. */
        public int read(byte[] nv12) {
            if (mStaticNv12 != null) {
                if (mStaticClosed) return 0x80004004;  // E_ABORT
                System.arraycopy(mStaticNv12, 0, nv12, 0, Math.min(nv12.length, mStaticNv12.length));
                try {
                    Thread.sleep(33);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                }
                return mStaticClosed ? 0x80004004 : 0;
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
            if (mStaticNv12 != null) {
                return mStaticClosed ? 0x80004004 : 0x80004001;  // E_ABORT / E_NOTIMPL
            }
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
            if (mStaticNv12 != null) {
                mStaticClosed = true;
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
        if (IMAGE_PICKER_ID.equals(id)) {
            return openImagePicker(id, width, height);
        }
        if (CameraImageFallback.enabled() && IntentChooser.HOST.available()) {
            int choice = IntentChooser.HOST.choose("camera", "Choose camera source",
                    new String[]{"PC Camera", "Choose image (Windows file picker)"},
                    new boolean[]{true, true}, () -> true);
            if (choice == 1) {
                return openImagePicker(id, width, height);
            }
            if (choice < 0) {
                throw new OpenException(id, 0x800704C7);
            }
        }
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

    /** Re-opens an already-selected {@code imageFile} at {@code width} x {@code height} without prompting. */
    public static Capture openImage(File imageFile, int width, int height) throws OpenException {
        if (imageFile == null) throw new OpenException(IMAGE_PICKER_ID, 0x800704C7);
        int evenW = Math.max(2, width & ~1);
        int evenH = Math.max(2, height & ~1);
        try {
            byte[] nv12 = CameraImageFallback.toNv12(imageFile, evenW, evenH);
            return new Capture(nv12, imageFile, evenW, evenH);
        } catch (IOException | RuntimeException e) {
            throw new OpenException(IMAGE_PICKER_ID, 0x800704C7);
        }
    }

    private static Capture openImagePicker(String id, int width, int height) throws OpenException {
        try {
            ImageSource source = sImageSource;
            String[] selected = source != null ? source.pick()
                    : WindowsHost.pickDocuments(DocumentPicker.OPEN, "Select an image", "", null,
                            "*.jpg;*.jpeg;*.png;*.bmp;*.webp;*.gif", null, false, false,
                            android.os.Process.myPid(), () -> true);
            if (selected == null || selected.length == 0 || selected[0] == null) {
                throw new OpenException(id, 0x800704C7);
            }
            return openImage(new File(Zygote.fromWindowsPath(selected[0])), width, height);
        } catch (OpenException e) {
            throw e;
        } catch (IOException | RuntimeException e) {
            throw new OpenException(id, 0x800704C7);
        }
    }

    private static native String[] nativeList();

    private static native String nativeStatus();

    private static native long nativeOpen(String id, int width, int height, int[] out);

    private static native int nativeRead(long handle, byte[] nv12);

    private static native int nativeFocus(long handle, int op, int[] out);

    private static native void nativeClose(long handle);
}
