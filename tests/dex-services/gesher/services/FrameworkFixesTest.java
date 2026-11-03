/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import java.io.File;
import java.io.RandomAccessFile;
import java.lang.reflect.Method;
import java.nio.channels.FileChannel;
import java.nio.channels.FileLock;
import java.nio.channels.OverlappingFileLockException;

/**
 * Small fixes on the boot class path, each of which failed in an app:
 *  - android.graphics.ColorMatrix (app/java, commit bf94f958): framework.jar does not
 *    have it, and an app's ColorMatrixColorFilter died with NoClassDefFoundError;
 *  - FileChannel.lock / tryLock / release (FileDispatcherImpl.lock0 / release0 over
 *    fcntl, compat/devsvc/devsvc_natives.cc, commits 93d60cff and d768300d): a blocking
 *    lock that hung or failed, as SharedPreferences' and Room's lock files take;
 *  - android.view.RenderNode and DisplayListCanvas (the pre-API-29 names, commit
 *    9ff23f33), which older UI libraries look up by reflection: present with the
 *    methods they call (not called here: android.graphics.RenderNode needs hwui).
 * Prints "FrameworkFixesTest all passed".
 */
public final class FrameworkFixesTest {
    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private static boolean near(float a, float b) {
        return Math.abs(a - b) < 1e-5f;
    }

    private static void colorMatrix() throws Exception {
        Class<?> type = Class.forName("android.graphics.ColorMatrix");
        Object m = type.getConstructor().newInstance();
        Method getArray = type.getMethod("getArray");
        float[] identity = (float[]) getArray.invoke(m);
        require(identity.length == 20 && identity[0] == 1 && identity[6] == 1 && identity[12] == 1
                && identity[18] == 1 && identity[4] == 0, "a new ColorMatrix is not the identity");
        type.getMethod("setSaturation", float.class).invoke(m, 0f);
        float[] gray = (float[]) getArray.invoke(m);
        for (int row = 0; row < 3; row++) {
            require(near(gray[row * 5], 0.213f) && near(gray[row * 5 + 1], 0.715f)
                    && near(gray[row * 5 + 2], 0.072f), "setSaturation(0) row " + row + " is wrong");
        }
        require(near(gray[18], 1f), "setSaturation(0) changed alpha");
        Object scale = type.getConstructor().newInstance();
        type.getMethod("setScale", float.class, float.class, float.class, float.class)
                .invoke(scale, 2f, 2f, 2f, 1f);
        type.getMethod("postConcat", type).invoke(m, scale);
        float[] scaled = (float[]) getArray.invoke(m);
        require(near(scaled[0], 0.426f) && near(scaled[6], 1.43f) && near(scaled[18], 1f),
                "postConcat(setScale(2)) is wrong");
        System.out.println("FrameworkFixesTest: ColorMatrix on the boot class path: identity, "
                + "setSaturation, setScale, postConcat");
    }

    private static void fileLocks() throws Exception {
        String data = System.getenv("ANDROID_DATA");
        File file = new File(data != null ? data : ".", "framework-fixes-lock.tmp");
        try (RandomAccessFile raf = new RandomAccessFile(file, "rw");
                FileChannel channel = raf.getChannel()) {
            raf.write(new byte[64]);
            FileLock whole = channel.lock();
            require(whole.isValid() && !whole.isShared(), "lock() is not an exclusive valid lock");
            boolean overlapped = false;
            try {
                channel.tryLock(0, 8, false);
            } catch (OverlappingFileLockException expected) {
                overlapped = true;
            }
            require(overlapped, "a second lock over the same range was granted");
            whole.release();
            require(!whole.isValid(), "release() left the lock valid");
            FileLock part = channel.tryLock(0, 16, true);
            require(part != null && part.isShared() && part.isValid(), "tryLock(shared) failed");
            FileLock other = channel.tryLock(32, 16, false);
            require(other != null && other.isValid(), "tryLock of a disjoint range failed");
            part.release();
            other.release();
            FileLock again = channel.lock(0, Long.MAX_VALUE, false);
            require(again.isValid(), "lock() after release failed");
            again.release();
        } finally {
            file.delete();
        }
        System.out.println("FrameworkFixesTest: FileChannel lock, tryLock (shared, ranges) and "
                + "release");
    }

    private static void legacyRenderNode() throws Exception {
        Class<?> view = Class.forName("android.view.View");
        Class<?> node = Class.forName("android.view.RenderNode");
        Class<?> canvas = Class.forName("android.view.DisplayListCanvas");
        Method create = node.getMethod("create", String.class, view);
        require(java.lang.reflect.Modifier.isStatic(create.getModifiers())
                && create.getReturnType() == node, "RenderNode.create(String, View) is not a factory");
        node.getMethod("setAlpha", float.class);
        node.getMethod("getAlpha");
        require(node.getMethod("start", int.class, int.class).getReturnType() == canvas,
                "RenderNode.start(int, int) does not return a DisplayListCanvas");
        node.getMethod("end", canvas);
        canvas.getMethod("drawRenderNode", node);
        require(Class.forName("android.graphics.Canvas").isAssignableFrom(canvas),
                "DisplayListCanvas is not a Canvas");
        System.out.println("FrameworkFixesTest: android.view.RenderNode and DisplayListCanvas "
                + "with create, start, end, drawRenderNode");
    }

    public static void main(String[] args) throws Exception {
        colorMatrix();
        fileLocks();
        legacyRenderNode();
        System.out.println("FrameworkFixesTest all passed");
    }
}
