/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.content.Intent;

import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.util.Collections;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/**
 * The document picker (ActivityTaskManagerService.start on ACTION_OPEN_DOCUMENT / _TREE,
 * commit 0f2f9eaa): the Windows dialog runs on a "document-picker" thread of its own and
 * startActivity returns at once, instead of holding the app's binder call (and its UI)
 * until the modal dialog closes. Also Zygote.fromWindowsPath, which maps the picked
 * Windows path to Gesher's path model.
 *
 * The real dialog is shown (no package resolves the intent, so the picker branch runs):
 * on a desktop it stays up, and the old code never returned. The process exits at the
 * end, closing it.
 */
public final class DocumentPickerTest {
    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private static Thread pickerThread() {
        for (Thread t : Thread.getAllStackTraces().keySet()) {
            if ("document-picker".equals(t.getName()) && t.isAlive()) return t;
        }
        return null;
    }

    private static void fromWindowsPath() throws Exception {
        Class<?> zygote = Class.forName("com.gesher.server.Zygote");
        Method from = zygote.getDeclaredMethod("fromWindowsPath", String.class);
        from.setAccessible(true);
        Method exePath = zygote.getDeclaredMethod("exePath");
        exePath.setAccessible(true);
        String exe = (String) exePath.invoke(null);
        require(exe != null && exe.length() > 1 && exe.charAt(1) == ':',
                "Zygote.exePath has no drive: " + exe);
        char own = Character.toLowerCase(exe.charAt(0));
        char other = own == 'q' ? 'r' : 'q';
        String same = (String) from.invoke(null, Character.toUpperCase(own) + ":\\Users\\a b\\f.txt");
        require("/Users/a b/f.txt".equals(same), "the own drive's path became " + same);
        String elsewhere = (String) from.invoke(null, Character.toUpperCase(other) + ":\\docs\\f.txt");
        require(("/mnt/" + other + "/docs/f.txt").equals(elsewhere),
                "another drive's path became " + elsewhere);
        String root = (String) from.invoke(null, other + ":");
        require(("/mnt/" + other + "/").equals(root), "a drive root became " + root);
        String plain = (String) from.invoke(null, "\\\\server\\share\\f.txt");
        require("/mnt/unc/server/share/f.txt".equals(plain), "a UNC path became " + plain);
        require(plain.equals(new java.io.File(plain).getAbsolutePath()),
                "Java File changed the picked UNC identity");
        require(plain.equals(java.nio.file.Paths.get(plain).normalize().toString()),
                "Java Path normalization changed the picked UNC identity");
        System.out.println("DocumentPickerTest: fromWindowsPath maps the own drive to /, "
                + "another to /mnt/<drive>");
    }

    public static void main(String[] args) throws Exception {
        // Zygote's and Binder's natives (gesher-app loads them in an app).
        System.loadLibrary("android_runtime");
        fromWindowsPath();

        Class<?> pmClass = Class.forName("com.gesher.server.PackageManagerService");
        Constructor<?> pmConstructor = pmClass.getDeclaredConstructor(java.util.Collection.class);
        pmConstructor.setAccessible(true);
        Object pm = pmConstructor.newInstance(Collections.emptyList());
        Class<?> atmsClass = Class.forName("com.gesher.server.ActivityTaskManagerService");
        Constructor<?> atmsConstructor = atmsClass.getDeclaredConstructor(pmClass);
        atmsConstructor.setAccessible(true);
        Object atms = atmsConstructor.newInstance(pm);
        Method start = null;
        for (Method m : atmsClass.getDeclaredMethods()) {
            if (m.getName().equals("start") && m.getParameterCount() == 8) start = m;
        }
        require(start != null, "no ActivityTaskManagerService.start(8 arguments)");
        start.setAccessible(true);
        final Method startMethod = start;

        String[] actions = {Intent.ACTION_OPEN_DOCUMENT, Intent.ACTION_OPEN_DOCUMENT_TREE};
        for (String action : actions) {
            Intent intent = new Intent(action);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            AtomicReference<Object> result = new AtomicReference<>();
            AtomicReference<Throwable> failure = new AtomicReference<>();
            CountDownLatch returned = new CountDownLatch(1);
            Thread caller = new Thread(() -> {
                try {
                    result.set(startMethod.invoke(atms, null, intent, null, "gesher.services",
                            null, null, 7, null));
                } catch (Throwable t) {
                    failure.set(t);
                } finally {
                    returned.countDown();
                }
            }, "binder-caller");
            caller.setDaemon(true);
            caller.start();
            // Android's startActivityForResult returns at once; 10 s is far beyond any
            // non-blocking start and far below a user closing a dialog.
            if (!returned.await(10, TimeUnit.SECONDS)) {
                System.out.println("DocumentPickerTest: FAIL startActivity(" + action
                        + ") is still blocked after 10 s (the picker runs on the caller's thread)");
                System.out.flush();
                Runtime.getRuntime().halt(1);
            }
            if (failure.get() != null) throw new AssertionError("start threw", failure.get());
            require(Integer.valueOf(0).equals(result.get()),
                    "startActivity(" + action + ") returned " + result.get() + ", not START_SUCCESS");
            // Reported, not required: a runner without an interactive desktop closes the
            // dialog at once (Show fails), and the thread is gone.
            Thread picker = null;
            for (int i = 0; i < 30 && picker == null; i++) {
                picker = pickerThread();
                if (picker == null) Thread.sleep(100);
            }
            System.out.println("DocumentPickerTest: " + action + " "
                    + (picker != null ? "dialog open on the document-picker thread"
                            : "dialog closed at once (no interactive desktop?)"));
            if (picker != null) {
                // The next action's dialog would share the screen: one at a time is enough.
                break;
            }
        }
        System.out.println("DocumentPickerTest: startActivity returns at once while the picker "
                + "is up; Windows paths mapped passed");
        System.out.flush();
        // The open dialog (a daemon thread in a modal loop) ends with the process.
        Runtime.getRuntime().halt(0);
    }
}
