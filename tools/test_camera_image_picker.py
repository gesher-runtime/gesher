#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Verify CameraImageFallback, DocumentPicker image-capture, and WindowsCamera image picker."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

SHIMS: dict[str, str] = {
    'android/graphics/Bitmap.java': """\
package android.graphics;
import java.io.IOException;
import java.io.OutputStream;
public final class Bitmap {
    public enum CompressFormat { JPEG, PNG, WEBP }
    public enum Config { ARGB_8888 }
    private final int width, height;
    private final int[] pixels;
    private boolean recycled;
    public Bitmap(int width, int height) {
        this.width = width;
        this.height = height;
        this.pixels = new int[Math.max(1, width * height)];
        for (int i = 0; i < this.pixels.length; i++) this.pixels[i] = 0xFF336699;
    }
    public static Bitmap createBitmap(int width, int height, Config config) {
        return new Bitmap(width, height);
    }
    public static Bitmap createScaledBitmap(Bitmap src, int dstWidth, int dstHeight, boolean filter) {
        return new Bitmap(dstWidth, dstHeight);
    }
    public int getWidth() { return width; }
    public int getHeight() { return height; }
    public boolean compress(CompressFormat format, int quality, OutputStream out) {
        try {
            out.write(new byte[]{(byte) 0xFF, (byte) 0xD8, (byte) 0xFF, (byte) 0xD9});
            return true;
        } catch (IOException e) {
            return false;
        }
    }
    public void getPixels(int[] pixels, int offset, int stride, int x, int y, int w, int h) {
        for (int row = 0; row < h; row++) {
            System.arraycopy(this.pixels, row * width, pixels, offset + row * stride, w);
        }
    }
    public void setPixel(int idx, int color) {
        if (idx >= 0 && idx < pixels.length) pixels[idx] = color;
    }
    public void recycle() { recycled = true; }
    public boolean isRecycled() { return recycled; }
}
""",
    'android/graphics/BitmapFactory.java': """\
package android.graphics;
public final class BitmapFactory {
    public static final class Options {
        public boolean inJustDecodeBounds;
        public int outWidth;
        public int outHeight;
        public int inSampleSize = 1;
    }
    public static Bitmap decodeFile(String pathName) {
        return pathName == null ? null : new Bitmap(320, 240);
    }
    public static Bitmap decodeFile(String pathName, Options opts) {
        if (pathName == null) return null;
        if (opts != null && opts.inJustDecodeBounds) {
            opts.outWidth = 320;
            opts.outHeight = 240;
            return null;
        }
        return new Bitmap(320, 240);
    }
}
""",
    'android/graphics/Color.java': """\
package android.graphics;
public final class Color {
    public static final int BLACK = 0xFF000000;
}
""",
    'android/graphics/Paint.java': """\
package android.graphics;
public final class Paint {
    public static final int FILTER_BITMAP_FLAG = 2;
    public Paint(int flags) {}
}
""",
    'android/graphics/RectF.java': """\
package android.graphics;
public final class RectF {
    public float left, top, right, bottom;
    public RectF(float l, float t, float r, float b) {
        left = l; top = t; right = r; bottom = b;
    }
}
""",
    'android/graphics/Canvas.java': """\
package android.graphics;
public final class Canvas {
    private final Bitmap target;
    public Canvas(Bitmap bitmap) { this.target = bitmap; }
    public void drawColor(int color) {}
    public void drawBitmap(Bitmap bitmap, Object src, RectF dst, Paint paint) {
        if (target != null) target.setPixel(0, 0xFF336699);
    }
}
""",
    'android/net/Uri.java': """\
package android.net;
import java.io.File;
import java.util.List;
import java.util.Objects;
public final class Uri {
    private final String raw, scheme, authority, path;
    private Uri(String raw, String scheme, String authority, String path) {
        this.raw = raw; this.scheme = scheme; this.authority = authority; this.path = path;
    }
    public static Uri parse(String s) {
        int c = s.indexOf("://");
        String scheme = c >= 0 ? s.substring(0, c) : null;
        String rest = c >= 0 ? s.substring(c + 3) : s;
        int slash = rest.indexOf('/');
        String auth = slash >= 0 ? rest.substring(0, slash) : rest;
        String path = slash >= 0 ? rest.substring(slash) : "";
        return new Uri(s, scheme, auth, path);
    }
    public static Uri fromFile(File file) {
        return new Uri("file://" + file.getPath(), "file", "", file.getPath());
    }
    public String getScheme() { return scheme; }
    public String getAuthority() { return authority; }
    public String getPath() { return path; }
    public List<String> getPathSegments() { return List.of(); }
    @Override public boolean equals(Object o) {
        return (o instanceof Uri) && Objects.equals(raw, ((Uri) o).raw);
    }
    @Override public int hashCode() { return Objects.hashCode(raw); }
    @Override public String toString() { return raw; }
}
""",
    'android/content/ClipDescription.java': """\
package android.content;
public final class ClipDescription {
    public static boolean compareMimeTypes(String concreteType, String desiredType) {
        if (desiredType == null || concreteType == null) return false;
        if ("*/*".equals(desiredType)) return true;
        int slash = desiredType.indexOf('/');
        if (slash > 0 && desiredType.endsWith("/*")) {
            return concreteType.startsWith(desiredType.substring(0, slash + 1));
        }
        return desiredType.equalsIgnoreCase(concreteType);
    }
}
""",
    'android/content/ClipData.java': """\
package android.content;
import android.net.Uri;
import java.util.ArrayList;
import java.util.List;
public final class ClipData {
    public static final class Item {
        public final Uri uri;
        public Item(Uri uri) { this.uri = uri; }
    }
    private final List<Item> items = new ArrayList<>();
    public ClipData(CharSequence label, String[] mimeTypes, Item item) { items.add(item); }
    public void addItem(Item item) { items.add(item); }
}
""",
    'android/os/RemoteException.java': """\
package android.os;
public class RemoteException extends Exception {
    public RemoteException(String message) { super(message); }
}
""",
    'android/content/res/AssetFileDescriptor.java': """\
package android.content.res;
import java.io.Closeable;
import java.io.IOException;
import java.io.OutputStream;
public class AssetFileDescriptor implements Closeable {
    private final OutputStream out;
    public AssetFileDescriptor(OutputStream out) { this.out = out; }
    public OutputStream createOutputStream() throws IOException { return out; }
    @Override public void close() throws IOException { if (out != null) out.close(); }
}
""",
    'android/content/ContentProviderClient.java': """\
package android.content;
import android.content.res.AssetFileDescriptor;
import android.net.Uri;
import android.os.RemoteException;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.OutputStream;
public class ContentProviderClient implements AutoCloseable {
    private final ContentResolver resolver;
    public ContentProviderClient(ContentResolver resolver) { this.resolver = resolver; }
    public AssetFileDescriptor openAssetFile(Uri uri, String mode, Object signal)
            throws RemoteException, FileNotFoundException {
        try {
            OutputStream out = resolver.openOutputStream(uri);
            return out == null ? null : new AssetFileDescriptor(out);
        } catch (IOException e) {
            throw new FileNotFoundException(e.getMessage());
        }
    }
    @Override public void close() {}
}
""",
    'android/content/ContentResolver.java': """\
package android.content;
import android.net.Uri;
import java.io.IOException;
import java.io.OutputStream;
public class ContentResolver {
    public OutputStream openOutputStream(Uri uri) throws IOException { return null; }
    public ContentProviderClient acquireContentProviderClient(Uri uri) {
        return new ContentProviderClient(this);
    }
}
""",
    'android/content/Intent.java': """\
package android.content;
import android.net.Uri;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Map;
public final class Intent {
    public static final String ACTION_OPEN_DOCUMENT = "android.intent.action.OPEN_DOCUMENT";
    public static final String ACTION_CREATE_DOCUMENT = "android.intent.action.CREATE_DOCUMENT";
    public static final String ACTION_OPEN_DOCUMENT_TREE = "android.intent.action.OPEN_DOCUMENT_TREE";
    public static final String ACTION_GET_CONTENT = "android.intent.action.GET_CONTENT";
    public static final String EXTRA_ALLOW_MULTIPLE = "android.intent.extra.ALLOW_MULTIPLE";
    public static final String EXTRA_LOCAL_ONLY = "android.intent.extra.LOCAL_ONLY";
    public static final String EXTRA_MIME_TYPES = "android.intent.extra.MIME_TYPES";
    public static final String EXTRA_TITLE = "android.intent.extra.TITLE";
    public static final int FLAG_GRANT_READ_URI_PERMISSION = 1;
    public static final int FLAG_GRANT_WRITE_URI_PERMISSION = 2;
    public static final int FLAG_GRANT_PERSISTABLE_URI_PERMISSION = 64;
    public static final int FLAG_GRANT_PREFIX_URI_PERMISSION = 128;

    private String action, type, pkg;
    private ComponentName component;
    private Uri data;
    private int flags;
    private final Map<String, Object> extras = new HashMap<>();

    public Intent() {}
    public Intent(String action) { this.action = action; }
    public Intent(Intent o) {
        this.action = o.action; this.type = o.type; this.data = o.data;
        this.flags = o.flags; this.extras.putAll(o.extras);
        this.pkg = o.pkg; this.component = o.component;
    }
    public String getAction() { return action; }
    public String getType() { return type; }
    public Uri getData() { return data; }
    public ComponentName getComponent() { return component; }
    public String getPackage() { return pkg; }
    public Intent setPackage(String pkg) { this.pkg = pkg; return this; }
    public Intent setComponent(ComponentName component) { this.component = component; return this; }
    public int getFlags() { return flags; }
    public Intent setData(Uri data) { this.data = data; return this; }
    public Intent setDataAndType(Uri data, String type) { this.data = data; this.type = type; return this; }
    public Intent addFlags(int flags) { this.flags |= flags; return this; }
    public void setClipData(ClipData clip) {}
    public Intent putExtra(String name, Object value) { extras.put(name, value); return this; }
    public Object getExtra(String name) { return extras.get(name); }
    public boolean hasExtra(String name) { return extras.containsKey(name); }
    public boolean getBooleanExtra(String name, boolean def) {
        Object v = extras.get(name); return v instanceof Boolean ? (Boolean) v : def;
    }
    public int getIntExtra(String name, int def) {
        Object v = extras.get(name); return v instanceof Integer ? (Integer) v : def;
    }
    public String getStringExtra(String name) {
        Object v = extras.get(name); return v instanceof String ? (String) v : null;
    }
    public String[] getStringArrayExtra(String name) {
        Object v = extras.get(name); return v instanceof String[] ? (String[]) v : null;
    }
    @SuppressWarnings("unchecked")
    public ArrayList<String> getStringArrayListExtra(String name) {
        Object v = extras.get(name); return v instanceof ArrayList ? (ArrayList<String>) v : null;
    }
    public <T> T getParcelableExtra(String name, Class<T> clazz) {
        Object v = extras.get(name);
        return clazz.isInstance(v) ? clazz.cast(v) : null;
    }
}
""",
    'android/content/ComponentName.java': """\
package android.content;
public final class ComponentName {
    private final String pkg, cls;
    public ComponentName(String pkg, String cls) { this.pkg = pkg; this.cls = cls; }
    public String getPackageName() { return pkg; }
    public String getClassName() { return cls; }
}
""",
    'android/provider/MediaStore.java': """\
package android.provider;
public final class MediaStore {
    public static final String ACTION_IMAGE_CAPTURE = "android.media.action.IMAGE_CAPTURE";
    public static final String ACTION_IMAGE_CAPTURE_SECURE = "android.media.action.IMAGE_CAPTURE_SECURE";
    public static final String ACTION_PICK_IMAGES = "android.provider.action.PICK_IMAGES";
    public static final String EXTRA_OUTPUT = "output";
    public static final String EXTRA_PICK_IMAGES_MAX = "android.provider.extra.PICK_IMAGES_MAX";
    public static int getPickImagesMaxLimit() { return 100; }
}
""",
    'android/provider/DocumentsContract.java': """\
package android.provider;
import android.net.Uri;
public final class DocumentsContract {
    public static final String EXTRA_INITIAL_URI = "android.provider.extra.INITIAL_URI";
    public static final class Document {
        public static final String MIME_TYPE_DIR = "vnd.android.document/directory";
    }
    public static boolean isTreeUri(Uri uri) { return false; }
    public static String getTreeDocumentId(Uri uri) { return ""; }
    public static String getDocumentId(Uri uri) { return ""; }
    public static Uri buildDocumentUri(String authority, String docId) {
        return Uri.parse("content://" + authority + "/document/" + docId);
    }
    public static Uri buildTreeDocumentUri(String authority, String docId) {
        return Uri.parse("content://" + authority + "/tree/" + docId);
    }
}
""",
    'android/os/Environment.java': """\
package android.os;
import java.io.File;
public final class Environment {
    public static final String DIRECTORY_DOWNLOADS = "Download";
    public static File getExternalStorageDirectory() { return new File("/nonexistent-sdcard"); }
    public static File getExternalStoragePublicDirectory(String type) { return new File("/nonexistent-sdcard/" + type); }
}
""",
    'android/os/FileUtils.java': """\
package android.os;
public final class FileUtils {
    public static String buildValidFatFilename(String name) { return name; }
}
""",
    'android/os/Process.java': """\
package android.os;
public final class Process {
    public static int myPid() { return 1; }
}
""",
    'android/util/Size.java': """\
package android.util;
public final class Size {
    private final int w, h;
    public Size(int w, int h) { this.w = w; this.h = h; }
    public int getWidth() { return w; }
    public int getHeight() { return h; }
}
""",
    'libcore/content/type/MimeMap.java': """\
package libcore.content.type;
import java.util.Set;
public final class MimeMap {
    private static final MimeMap INSTANCE = new MimeMap();
    public static MimeMap getDefault() { return INSTANCE; }
    public String guessExtensionFromMimeType(String mimeType) {
        return "image/jpeg".equals(mimeType) ? "jpg" : null;
    }
    public String guessMimeTypeFromExtension(String extension) {
        if ("jpg".equals(extension) || "jpeg".equals(extension)) return "image/jpeg";
        if ("png".equals(extension)) return "image/png";
        return "application/octet-stream";
    }
    public Set<String> extensions() { return Set.of("jpg", "jpeg", "png"); }
}
""",
    'com/gesher/server/Device.java': """\
package com.gesher.server;
final class Device {
    static String sTestDataDir;
    static String dataDir() { return sTestDataDir; }
}
""",
    'com/gesher/server/WindowsHost.java': """\
package com.gesher.server;
import java.io.IOException;
import java.util.function.BooleanSupplier;
final class WindowsHost {
    static boolean available() { return false; }
    static boolean isLocalDocument(String windowsPath) { return true; }
    static String[] pickDocuments(int mode, String title, String suggestedName, String initialDir,
            String filter, String extension, boolean multiple, boolean localOnly,
            int ownerPid, BooleanSupplier active) throws IOException {
        return null;
    }
}
""",
    'com/gesher/server/DocumentsStorageProvider.java': """\
package com.gesher.server;
import java.io.File;
import java.io.FileNotFoundException;
final class DocumentsStorageProvider {
    static final String AUTHORITY = "com.gesher.documents";
    final Object registration = new Object();
    static File selectedFile(String path) { return new File(path); }
    static File file(String id) throws FileNotFoundException { return new File(id); }
    static String mimeType(File file) {
        String n = file.getName().toLowerCase();
        if (n.endsWith(".jpg") || n.endsWith(".jpeg")) return "image/jpeg";
        if (n.endsWith(".png")) return "image/png";
        return "application/octet-stream";
    }
    String createDocument(String parent, String mimeType, String name) { return new File(parent, name).getPath(); }
}
""",
    'com/gesher/server/Zygote.java': """\
package com.gesher.server;
final class Zygote {
    static String windowsPath(String p) { return p; }
    static String fromWindowsPath(String p) { return p; }
}
""",
    'com/gesher/server/SystemServer.java': """\
package com.gesher.server;
final class SystemServer {
    static void log(String msg) {}
}
""",
    'com/gesher/server/UriGrants.java': """\
package com.gesher.server;
import android.net.Uri;
final class UriGrants {
    static final class Grant {}
    Grant prepareFromProvider(Object reg, String target, Uri uri, int flags) { return new Grant(); }
}
""",
    'com/gesher/server/IntentChooser.java': """\
package com.gesher.server;
import java.util.function.BooleanSupplier;
final class IntentChooser {
    interface Dialog {
        boolean available();
        int choose(String requester, String title, String[] labels, boolean[] enabled, BooleanSupplier active);
    }
    static final Dialog HOST = new Dialog() {
        @Override public boolean available() { return false; }
        @Override public int choose(String requester, String title, String[] labels, boolean[] enabled, BooleanSupplier active) {
            return -1;
        }
    };
}
""",
}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home', type=Path, required=True)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix='gesher-camera-picker-') as temporary:
        work = Path(temporary)
        src_dir = work / 'src'
        classes_dir = work / 'classes'
        run_tmp = work / 'run'
        src_dir.mkdir()
        classes_dir.mkdir()
        run_tmp.mkdir()

        sources: list[str] = []
        for rel, content in SHIMS.items():
            target = src_dir / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(content, encoding='utf-8')
            sources.append(str(target))

        for prod in ('CameraImageFallback.java', 'DocumentPicker.java', 'WindowsCamera.java'):
            sources.append(str(ROOT / 'app/java/com/gesher/server' / prod))
        sources.append(str(ROOT / 'tests/java-performance/CameraImagePickerHost.java'))

        sources_list = work / 'sources.txt'
        sources_list.write_text('\n'.join(sources), encoding='utf-8')

        subprocess.run(
            [
                str(args.java_home / 'bin/javac'),
                '-J-XX:-UsePerfData',
                '-source', '17',
                '-target', '17',
                '-Xlint:-options',
                '-encoding', 'UTF-8',
                '-d', str(classes_dir),
                '@' + str(sources_list),
            ],
            check=True,
            timeout=300,
        )
        subprocess.run(
            [
                str(args.java_home / 'bin/java'),
                '-XX:-UsePerfData',
                '-Xmx64m',
                '-cp', str(classes_dir),
                'com.gesher.server.CameraImagePickerHost',
                str(run_tmp),
            ],
            check=True,
            timeout=60,
        )


if __name__ == '__main__':
    main()
