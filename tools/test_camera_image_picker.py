#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Run production CameraImageFallback, CameraImageSource, CameraSourceService and
WindowsCamera's selected-image capture (tests/java-performance/CameraImagePickerHost.java).

The framework classes they use (graphics, Binder, Parcel, ServiceManager) and the server's
Windows dialogs are shims below: the Binder shim runs a transaction in the same JVM, so the
client and the service exchange real Parcels and a real file's descriptor. No Windows dialog,
camera or ART runtime is involved; the live dialogs are covered by the manual checks in
docs/devices.md.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / 'app/java/com/gesher/server'

SHIMS: dict[str, str] = {
    'android/Manifest.java': '''package android;
public final class Manifest { public static final class permission {
    public static final String CAMERA = "android.permission.CAMERA"; } }
''',
    'android/content/pm/PackageManager.java': '''package android.content.pm;
public abstract class PackageManager { public static final int PERMISSION_GRANTED = 0; }
''',
    'android/util/Size.java': '''package android.util;
public final class Size {
    private final int w, h;
    public Size(int w, int h) { this.w = w; this.h = h; }
    public int getWidth() { return w; }
    public int getHeight() { return h; }
}
''',
    'android/graphics/Bitmap.java': '''package android.graphics;
public final class Bitmap {
    public enum Config { ARGB_8888 }
    private final int width, height;
    private final int[] pixels;
    Bitmap(int width, int height) {
        this.width = width; this.height = height;
        pixels = new int[Math.max(1, width * height)];
    }
    public static Bitmap createBitmap(int width, int height, Config config) { return new Bitmap(width, height); }
    public int getWidth() { return width; }
    public int getHeight() { return height; }
    void fill(int color) { java.util.Arrays.fill(pixels, color); }
    public void getPixels(int[] out, int offset, int stride, int x, int y, int w, int h) {
        for (int row = 0; row < h; row++) System.arraycopy(pixels, row * width, out, offset + row * stride, w);
    }
    public void recycle() {}
}
''',
    'android/graphics/BitmapFactory.java': '''package android.graphics;
/** Any non-empty input decodes as a 320x240 image (the shim has no codecs). */
public final class BitmapFactory {
    public static final class Options {
        public boolean inJustDecodeBounds;
        public int outWidth, outHeight, inSampleSize = 1;
    }
    public static Bitmap decodeByteArray(byte[] data, int offset, int length, Options opts) {
        if (length <= 0) return null;
        if (opts != null && opts.inJustDecodeBounds) { opts.outWidth = 320; opts.outHeight = 240; return null; }
        return new Bitmap(320, 240);
    }
}
''',
    'android/graphics/Color.java': '''package android.graphics;
public final class Color { public static final int BLACK = 0xFF000000; }
''',
    'android/graphics/Paint.java': '''package android.graphics;
public final class Paint { public static final int FILTER_BITMAP_FLAG = 2; public Paint(int flags) {} }
''',
    'android/graphics/RectF.java': '''package android.graphics;
public final class RectF { public RectF(float l, float t, float r, float b) {} }
''',
    'android/graphics/Canvas.java': '''package android.graphics;
public final class Canvas {
    private final Bitmap target;
    public Canvas(Bitmap bitmap) { target = bitmap; }
    public void drawColor(int color) { target.fill(color); }
    public void drawBitmap(Bitmap bitmap, Object src, RectF dst, Paint paint) { target.fill(0xFF336699); }
}
''',
    'android/os/RemoteException.java': '''package android.os;
public class RemoteException extends Exception {}
''',
    'android/os/IBinder.java': '''package android.os;
public interface IBinder {
    int FIRST_CALL_TRANSACTION = 1;
    boolean transact(int code, Parcel data, Parcel reply, int flags) throws RemoteException;
}
''',
    'android/os/Binder.java': '''package android.os;
/** A transaction runs in this JVM; tests set the caller's pid. */
public class Binder implements IBinder {
    public static volatile int callingPid;
    public static int getCallingPid() { return callingPid; }
    public static int getCallingUid() { return 10000 + callingPid; }
    protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
        return false;
    }
    public final boolean transact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
        return onTransact(code, data, reply, flags);
    }
}
''',
    'android/os/Parcel.java': '''package android.os;
import java.util.ArrayList;
import java.util.List;
public final class Parcel {
    private final List<Object> values = new ArrayList<>();
    private int read;
    public static Parcel obtain() { return new Parcel(); }
    public void recycle() {}
    private Object next() { return values.get(read++); }
    void writeObject(Object o) { values.add(o); }
    Object readObject() { return next(); }
    public void writeInterfaceToken(String d) { values.add(d); }
    public void enforceInterface(String d) {
        if (!d.equals(next())) throw new SecurityException("Binder invocation to an incorrect interface");
    }
    public void writeString(String s) { values.add(s); }
    public String readString() { return (String) next(); }
    public void writeBoolean(boolean b) { values.add(b); }
    public boolean readBoolean() { return (Boolean) next(); }
    public void writeInt(int i) { values.add(i); }
    public int readInt() { return (Integer) next(); }
    public void writeNoException() { values.add(0); }
    public void writeException(Exception e) { values.add(-1); values.add(e); }
    public void readException() {
        if ((Integer) next() != 0) throw (RuntimeException) next();
    }
}
''',
    'android/os/Parcelable.java': '''package android.os;
public interface Parcelable { int PARCELABLE_WRITE_RETURN_VALUE = 1; }
''',
    'android/os/ParcelFileDescriptor.java': '''package android.os;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.util.concurrent.atomic.AtomicInteger;
/** Counts open descriptors; writing a return value hands over a new one and closes this. */
public final class ParcelFileDescriptor implements java.io.Closeable {
    public static final int MODE_READ_ONLY = 0x10000000;
    public static final AtomicInteger OPEN = new AtomicInteger();
    final File file;
    private boolean closed;
    private ParcelFileDescriptor(File file) { this.file = file; OPEN.incrementAndGet(); }
    public static ParcelFileDescriptor open(File file, int mode) throws FileNotFoundException {
        if (mode != MODE_READ_ONLY) throw new IllegalArgumentException("read-only expected");
        if (!file.isFile()) throw new FileNotFoundException(file.getPath());
        return new ParcelFileDescriptor(file);
    }
    public void writeToParcel(Parcel out, int flags) {
        out.writeObject(new ParcelFileDescriptor(file));
        if ((flags & Parcelable.PARCELABLE_WRITE_RETURN_VALUE) != 0) close();
    }
    @Override public synchronized void close() {
        if (closed) throw new IllegalStateException("closed twice");
        closed = true;
        OPEN.decrementAndGet();
    }
    public static final class Creator {
        public ParcelFileDescriptor createFromParcel(Parcel in) { return (ParcelFileDescriptor) in.readObject(); }
    }
    public static final Creator CREATOR = new Creator();
    public static final class AutoCloseInputStream extends FileInputStream {
        private final ParcelFileDescriptor pfd;
        public AutoCloseInputStream(ParcelFileDescriptor pfd) throws FileNotFoundException {
            super(pfd.file);
            this.pfd = pfd;
        }
        @Override public void close() throws IOException { super.close(); pfd.close(); }
    }
}
''',
    'android/os/ServiceManager.java': '''package android.os;
public final class ServiceManager {
    public static final java.util.Map<String, IBinder> services = new java.util.HashMap<>();
    public static IBinder getService(String name) { return services.get(name); }
}
''',
    'android/os/SystemClock.java': '''package android.os;
public final class SystemClock { public static long uptimeMillis() { return System.nanoTime() / 1000000; } }
''',
    'android/os/Process.java': '''package android.os;
public final class Process { public static int myPid() { return 1; } }
''',
    'android/os/LocaleList.java': '''package android.os;
public final class LocaleList { public String toLanguageTags() { return "he-IL,en-US"; } }
''',
    'com/gesher/app/locale/SystemLocale.java': '''package com.gesher.app.locale;
public final class SystemLocale {
    public static android.os.LocaleList locales() { return new android.os.LocaleList(); }
}
''',
    'com/gesher/server/Device.java': '''package com.gesher.server;
final class Device { static String dataDir() { return null; } }
''',
    'com/gesher/server/IntentChooser.java': '''package com.gesher.server;
import java.util.function.BooleanSupplier;
final class IntentChooser {
    static final int UNAVAILABLE = -3;
    interface Dialog {
        boolean available();
        int choose(String requester, String title, String[] labels, boolean[] enabled, BooleanSupplier active);
    }
    static final Dialog HOST = new Dialog() {
        public boolean available() { return false; }
        public int choose(String r, String t, String[] l, boolean[] e, BooleanSupplier a) { return UNAVAILABLE; }
    };
}
''',
    'com/gesher/server/WindowsHost.java': '''package com.gesher.server;
final class WindowsHost {
    static String[] pickDocuments(int mode, String title, String suggestedName, String initialDirectory,
            String filter, String extension, boolean multiple, boolean localOnly, int ownerPid,
            java.util.function.BooleanSupplier active) throws java.io.IOException {
        throw new java.io.IOException("no Windows picker in host tests");
    }
}
''',
    'com/gesher/server/DocumentPicker.java': '''package com.gesher.server;
final class DocumentPicker { static final int OPEN = 0; }
''',
    'com/gesher/server/Zygote.java': '''package com.gesher.server;
final class Zygote { static String fromWindowsPath(String path) { return path; } }
''',
    'com/gesher/server/SystemServer.java': '''package com.gesher.server;
final class SystemServer { static void log(String message) { System.out.println("server: " + message); } }
''',
    'com/gesher/server/CameraService.java': '''package com.gesher.server;
final class CameraService { static void log(String message) { System.out.println("camera: " + message); } }
''',
    'com/gesher/server/CallerIdentity.java': '''package com.gesher.server;
final class CallerIdentity {
    boolean isSystem() { return false; }
    String callerPackage() { throw new SecurityException("host test"); }
    String packageForPid(int pid) { return null; }
}
''',
    'com/gesher/server/ActivityManagerService.java': '''package com.gesher.server;
final class ActivityManagerService {
    CallerIdentity callers() { return new CallerIdentity(); }
    int checkPermission(String permission, int pid, int uid) { return -1; }
}
''',
}

PRODUCTION = [
    SERVER / 'CameraImageFallback.java',
    SERVER / 'CameraImageSource.java',
    SERVER / 'CameraSourceService.java',
    SERVER / 'WindowsCamera.java',
    ROOT / 'app/java/com/gesher/app/locale/UiMessages.java',
]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home', type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='gesher-camera-picker-') as temporary:
        work = Path(temporary)
        src = work / 'src'
        classes = work / 'classes'
        run = work / 'run'
        classes.mkdir()
        run.mkdir()
        sources = []
        for relative, text in SHIMS.items():
            path = src / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding='utf-8')
            sources.append(str(path))
        sources += [str(path) for path in PRODUCTION]
        sources.append(str(ROOT / 'tests/java-performance/CameraImagePickerHost.java'))
        listing = work / 'sources.txt'
        listing.write_text('\n'.join(sources), encoding='utf-8')
        subprocess.run([str(args.java_home / 'bin/javac'), '-J-XX:-UsePerfData', '-source', '17',
                        '-target', '17', '-Xlint:-options', '-encoding', 'UTF-8', '-d', str(classes),
                        '@' + str(listing)], check=True, timeout=300)
        subprocess.run([str(args.java_home / 'bin/java'), '-XX:-UsePerfData', '-Xmx64m', '-cp',
                        str(classes), 'com.gesher.server.CameraImagePickerHost', str(run)],
                       check=True, timeout=60)


if __name__ == '__main__':
    main()
