/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.Manifest;
import android.content.pm.PackageManager;
import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.ParcelFileDescriptor;
import android.os.Parcelable;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.os.SystemClock;

import com.gesher.app.locale.SystemLocale;
import com.gesher.app.locale.UiMessages;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.util.function.BooleanSupplier;

/**
 * {@code gesher.camera_source}: the system server's side of the camera image-picker setting
 * ({@link CameraImageFallback}, {@link CameraImageSource}). App processes run the camera
 * service themselves but cannot read data/system or the PC's files (and, isolated, show no
 * Windows dialogs of the server's), so they ask here: whether the setting is on, and, for a
 * camera an app opens, the user's answer — the PC camera, or an image chosen with the Windows
 * file picker, returned as a read-only descriptor. Choosing needs the caller's CAMERA grant,
 * as opening the camera does. A plain Binder with its own transaction codes, as
 * {@link GesherService}.
 */
public final class CameraSourceService extends Binder {
    public static final String NAME = "gesher.camera_source";
    static final String DESCRIPTOR = "com.gesher.ICameraSource";
    static final int ENABLED = IBinder.FIRST_CALL_TRANSACTION;
    static final int CHOOSE = IBinder.FIRST_CALL_TRANSACTION + 1;

    /** The Windows question and file picker (IntentChooser's dialog, WindowsHost's picker). */
    interface Ui {
        boolean available();

        /** The chosen label's index, {@link IntentChooser#UNAVAILABLE} or another negative (cancel). */
        int ask(String requester, String title, String[] labels, BooleanSupplier active);

        /** The selected file's Windows path, or null when the user cancels. */
        String pick(String title, int ownerPid, BooleanSupplier active) throws IOException;

        /**
         * The selected file, as the document picker admits a selection: canonical, local, and
         * not in Android/data, obb or sandbox (other apps' private storage).
         */
        File resolve(String windowsPath) throws IOException;
    }

    static final Ui WINDOWS = new Ui() {
        @Override public boolean available() {
            return IntentChooser.HOST.available();
        }

        @Override public int ask(String requester, String title, String[] labels,
                BooleanSupplier active) {
            boolean[] enabled = new boolean[labels.length];
            java.util.Arrays.fill(enabled, true);
            return IntentChooser.HOST.choose(requester, title, labels, enabled, active);
        }

        @Override public String pick(String title, int ownerPid, BooleanSupplier active)
                throws IOException {
            String[] selected = WindowsHost.pickDocuments(DocumentPicker.OPEN, title, "", null,
                    "*.jpg;*.jpeg;*.png;*.bmp;*.webp;*.gif", null, false, true, ownerPid, active);
            return selected == null || selected.length == 0 ? null : selected[0];
        }

        @Override public File resolve(String windowsPath) throws IOException {
            File file = DocumentsStorageProvider.selectedFile(windowsPath);
            if (DocumentPicker.blockedAndroidDirectory(file)) {
                throw new IOException("Android data/obb is not selectable");
            }
            if (!WindowsHost.isLocalDocument(Zygote.windowsPath(file.getPath()))) {
                throw new IOException("the selected file is not available locally");
            }
            return file;
        }
    };

    /** At most one question at a time: each holds a binder thread until the user answers. */
    private final java.util.concurrent.Semaphore mQuestion = new java.util.concurrent.Semaphore(1);

    /** Who is calling: an attached app with its CAMERA grant (host tests replace it). */
    interface Callers {
        /** The caller's package; SecurityException for an unknown caller. */
        String packageOf(int pid, int uid);

        boolean cameraGranted(int pid, int uid);

        /** The process is still attached (the question is dismissed when it is gone). */
        boolean attached(int pid);
    }

    /** The answer to {@link #CHOOSE}: the camera, a selected file, or a cancel. */
    static final class Answer {
        final int kind;  // CameraImageSource.Selection.CAMERA, IMAGE or CANCELLED
        final File image;

        Answer(int kind, File image) {
            this.kind = kind;
            this.image = image;
        }
    }

    private final Callers mCallers;
    private final Ui mUi;
    private final BooleanSupplier mEnabled;

    CameraSourceService(ActivityManagerService am) {
        this(new Callers() {
            @Override public String packageOf(int pid, int uid) {
                CallerIdentity callers = am.callers();
                return callers.isSystem() ? "android" : callers.callerPackage();
            }

            @Override public boolean cameraGranted(int pid, int uid) {
                return am.checkPermission(Manifest.permission.CAMERA, pid, uid)
                        == PackageManager.PERMISSION_GRANTED;
            }

            @Override public boolean attached(int pid) {
                return pid == android.os.Process.myPid() || am.callers().packageForPid(pid) != null;
            }
        }, WINDOWS, CameraImageFallback::enabled);
    }

    CameraSourceService(Callers callers, Ui ui, BooleanSupplier enabled) {
        mCallers = callers;
        mUi = ui;
        mEnabled = enabled;
    }

    private static String text(String key) {
        return UiMessages.text(key, SystemLocale.locales().toLanguageTags());
    }

    /** The user's answer for {@code pid}'s open of a camera ({@code physical}: a PC camera). */
    Answer answer(int pid, int uid, String cameraId, boolean physical) throws IOException {
        if (!mEnabled.getAsBoolean()) {
            return new Answer(physical ? CameraImageSource.Selection.CAMERA
                    : CameraImageSource.Selection.CANCELLED, null);
        }
        String requester = mCallers.packageOf(pid, uid);
        if (!mCallers.cameraGranted(pid, uid)) {
            throw new SecurityException("Choosing a camera source requires the CAMERA permission");
        }
        // A question waits for the user on this binder thread: one at a time, so callers
        // cannot pile dialogs up or use the server's binder threads up.
        if (!mQuestion.tryAcquire()) {
            SystemServer.log("camera source: " + requester + ": another question is open");
            return new Answer(CameraImageSource.Selection.CANCELLED, null);
        }
        try {
            return ask(requester, pid, cameraId, physical);
        } finally {
            mQuestion.release();
        }
    }

    private Answer ask(String requester, int pid, String cameraId, boolean physical)
            throws IOException {
        BooleanSupplier active = () -> mCallers.attached(pid);
        if (physical) {
            if (!mUi.available()) return new Answer(CameraImageSource.Selection.CAMERA, null);
            int index = mUi.ask(requester, text("camera_source_title"),
                    new String[] {text("camera_source_camera"), text("camera_source_image")}, active);
            // No dialog on this desktop: the camera, as without the setting.
            if (index == IntentChooser.UNAVAILABLE || index == 0) {
                return new Answer(CameraImageSource.Selection.CAMERA, null);
            }
            if (index != 1) return new Answer(CameraImageSource.Selection.CANCELLED, null);
        }
        String path = mUi.pick(text("camera_pick_title"), pid, active);
        if (path == null || !active.getAsBoolean()) {
            return new Answer(CameraImageSource.Selection.CANCELLED, null);
        }
        File file;
        try {
            file = mUi.resolve(path);
        } catch (IOException | IllegalArgumentException e) {
            SystemServer.log("camera source: " + requester + ": the selection is refused: "
                    + e.getMessage());
            return new Answer(CameraImageSource.Selection.CANCELLED, null);
        }
        // Only an image of a format Android decodes reaches the app (the picker's filter is
        // a suggestion the user can change); the user's path stays out of the log.
        if (!file.isFile() || file.length() > CameraImageSource.MAX_IMAGE_BYTES
                || !isImage(file)) {
            SystemServer.log("camera source: " + requester + ": " + file.getName()
                    + " is not a JPEG, PNG, GIF, BMP or WebP image of at most "
                    + CameraImageSource.MAX_IMAGE_BYTES + " bytes");
            return new Answer(CameraImageSource.Selection.CANCELLED, null);
        }
        SystemServer.log("camera source: " + requester + ", camera " + cameraId
                + ": a selected image (" + file.length() + " bytes)");
        return new Answer(CameraImageSource.Selection.IMAGE, file);
    }

    /** The file starts as a JPEG, PNG, GIF, BMP or WebP image does. */
    static boolean isImage(File file) {
        byte[] h = new byte[12];
        int n = 0;
        try (InputStream in = new java.io.FileInputStream(file)) {
            int r;
            while (n < h.length && (r = in.read(h, n, h.length - n)) > 0) n += r;
        } catch (IOException e) {
            return false;
        }
        if (n >= 3 && (h[0] & 0xFF) == 0xFF && (h[1] & 0xFF) == 0xD8 && (h[2] & 0xFF) == 0xFF) {
            return true;  // JPEG
        }
        if (n >= 8 && (h[0] & 0xFF) == 0x89 && h[1] == 'P' && h[2] == 'N' && h[3] == 'G') return true;
        if (n >= 6 && h[0] == 'G' && h[1] == 'I' && h[2] == 'F' && h[3] == '8') return true;
        if (n >= 2 && h[0] == 'B' && h[1] == 'M') return true;
        return n >= 12 && h[0] == 'R' && h[1] == 'I' && h[2] == 'F' && h[3] == 'F'
                && h[8] == 'W' && h[9] == 'E' && h[10] == 'B' && h[11] == 'P';
    }

    @Override
    protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
            throws RemoteException {
        if (code != ENABLED && code != CHOOSE) return super.onTransact(code, data, reply, flags);
        data.enforceInterface(DESCRIPTOR);
        if (code == ENABLED) {
            reply.writeNoException();
            reply.writeBoolean(mEnabled.getAsBoolean());
            return true;
        }
        String cameraId = data.readString();
        boolean physical = data.readBoolean();
        int pid = Binder.getCallingPid();
        int uid = Binder.getCallingUid();
        try {
            Answer answer = answer(pid, uid, cameraId, physical);
            ParcelFileDescriptor image = answer.image == null ? null
                    : ParcelFileDescriptor.open(answer.image, ParcelFileDescriptor.MODE_READ_ONLY);
            reply.writeNoException();
            reply.writeInt(answer.kind);
            if (image != null) image.writeToParcel(reply, Parcelable.PARCELABLE_WRITE_RETURN_VALUE);
        } catch (SecurityException e) {
            reply.writeException(e);
        } catch (IOException e) {
            SystemServer.log("camera source: " + e);
            reply.writeNoException();
            reply.writeInt(CameraImageSource.Selection.CANCELLED);
        }
        return true;
    }

    /** {@link CameraImageSource}'s side in an app process: this service over Binder. */
    static final CameraImageSource.Host CLIENT = new CameraImageSource.Host() {
        private volatile boolean mChecked;
        private volatile long mCheckedAt;
        private volatile boolean mEnabled;

        private IBinder service() {
            try {
                return ServiceManager.getService(NAME);
            } catch (RuntimeException | LinkageError unavailable) {
                return null;  // not Gesher's runtime
            }
        }

        @Override public boolean enabled() {
            // The camera list is read often; the setting at most once a second.
            long now = SystemClock.uptimeMillis();
            if (mChecked && now - mCheckedAt < 1000) return mEnabled;
            boolean enabled = false;
            IBinder service = service();
            if (service != null) {
                Parcel data = Parcel.obtain();
                Parcel reply = Parcel.obtain();
                try {
                    data.writeInterfaceToken(DESCRIPTOR);
                    service.transact(ENABLED, data, reply, 0);
                    reply.readException();
                    enabled = reply.readBoolean();
                } catch (RemoteException | RuntimeException e) {
                    enabled = false;
                } finally {
                    data.recycle();
                    reply.recycle();
                }
            }
            mEnabled = enabled;
            mCheckedAt = now;
            mChecked = true;
            return enabled;
        }

        @Override public CameraImageSource.Selection choose(String cameraId, boolean physical) {
            IBinder service = service();
            if (service == null) {
                return physical ? CameraImageSource.Selection.USE_CAMERA
                        : CameraImageSource.Selection.NONE;
            }
            Parcel data = Parcel.obtain();
            Parcel reply = Parcel.obtain();
            try {
                data.writeInterfaceToken(DESCRIPTOR);
                data.writeString(cameraId);
                data.writeBoolean(physical);
                service.transact(CHOOSE, data, reply, 0);
                reply.readException();
                int kind = reply.readInt();
                if (kind == CameraImageSource.Selection.CAMERA) {
                    return CameraImageSource.Selection.USE_CAMERA;
                }
                if (kind != CameraImageSource.Selection.IMAGE) return CameraImageSource.Selection.NONE;
                ParcelFileDescriptor image = ParcelFileDescriptor.CREATOR.createFromParcel(reply);
                long size = image.getStatSize();
                // The stream owns the descriptor and closes it once.
                try (InputStream in = new ParcelFileDescriptor.AutoCloseInputStream(image)) {
                    return new CameraImageSource.Selection(CameraImageSource.Selection.IMAGE,
                            read(in, size));
                }
            } catch (SecurityException e) {
                // Not an app the server knows, or no CAMERA grant: no question was asked, and
                // a PC camera opens as without the setting (its own permission check applies).
                CameraService.log("camera " + cameraId + ": choosing its source: " + e);
                return physical ? CameraImageSource.Selection.USE_CAMERA
                        : CameraImageSource.Selection.NONE;
            } catch (RemoteException | IOException | RuntimeException | OutOfMemoryError e) {
                CameraService.log("camera " + cameraId + ": choosing its source: " + e);
                return CameraImageSource.Selection.NONE;
            } finally {
                data.recycle();
                reply.recycle();
            }
        }
    };

    /**
     * The descriptor's {@code size} bytes (its file's size when it was sent), read into one
     * array of that size: at most {@link CameraImageSource#MAX_IMAGE_BYTES}.
     */
    static byte[] read(InputStream in, long size) throws IOException {
        if (size <= 0 || size > CameraImageSource.MAX_IMAGE_BYTES) {
            throw new IOException("the selected image has " + size + " bytes (at most "
                    + CameraImageSource.MAX_IMAGE_BYTES + ")");
        }
        byte[] bytes = new byte[(int) size];
        int at = 0;
        int n;
        while (at < bytes.length && (n = in.read(bytes, at, bytes.length - at)) > 0) at += n;
        if (at != bytes.length) throw new IOException("the selected image changed while read");
        return bytes;
    }
}
