/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.BooleanSupplier;

/** Actual Windows COM open/save/tree dialogs, lifetime cancellation and native path mapping. */
public final class DocumentPickerTest {
    private static void require(boolean value, String why) {
        if (!value) throw new AssertionError(why);
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

    private static void cancelDialog(Method pick, int mode, boolean multiple) throws Exception {
        AtomicInteger polls = new AtomicInteger();
        AtomicReference<Throwable> failure = new AtomicReference<>();
        AtomicReference<Object> selected = new AtomicReference<>();
        long start = System.nanoTime();
        AtomicLong pumping = new AtomicLong();
        BooleanSupplier lifetime = () -> {
            int count = polls.incrementAndGet();
            // The first two polls admit the request and Show. Start cancellation
            // from the timer poll so slow COM initialization cannot fake retirement.
            if (count >= 3) pumping.compareAndSet(0, System.nanoTime());
            return pumping.get() == 0 || System.nanoTime() - pumping.get() < TimeUnit.MILLISECONDS.toNanos(800);
        };
        Thread worker = new Thread(() -> {
            try {
                selected.set(pick.invoke(null, mode, "Gesher document picker cancellation fixture",
                        "new-fixture-file.txt", null, "*.txt", "txt", multiple,
                        true, android.os.Process.myPid(), lifetime));
            } catch (InvocationTargetException error) { failure.set(error.getCause()); }
            catch (Throwable error) { failure.set(error); }
        }, "native-document-dialog-fixture");
        worker.setDaemon(true);
        worker.start();
        worker.join(10000);
        require(!worker.isAlive(), "Native document dialog did not close after its owner retired");
        if (failure.get() != null) throw new AssertionError("Actual Windows dialog failed", failure.get());
        require(selected.get() == null, "Cancelled dialog returned selected paths");
        require(polls.get() >= 4 && System.nanoTime() - start >= TimeUnit.MILLISECONDS.toNanos(800),
                "Dialog closed without exercising its native message-loop lifetime timer");
        System.out.println("DocumentPickerTest: native mode " + mode + " cancelled and worker retired");
    }

    private static void locality(Class<?> host, String[] nonlocalPaths) throws Exception {
        Method local = host.getDeclaredMethod("isLocalDocument", String.class); local.setAccessible(true);
        Method windows = Class.forName("com.gesher.server.Zygote").getDeclaredMethod("windowsPath", String.class);
        java.nio.file.Path directory = java.nio.file.Files.createTempDirectory("gesher-local-document-");
        java.nio.file.Path file = directory.resolve("owned.txt");
        try {
            java.nio.file.Files.writeString(file, "real local test document");
            require((boolean)local.invoke(null, windows.invoke(null, directory.toString())),
                    "Actual local temporary directory was not verified by Windows");
            require((boolean)local.invoke(null, windows.invoke(null, file.toString())),
                    "Actual local file was not verified by Windows");
            require(!(boolean)local.invoke(null, windows.invoke(null, directory.resolve("absent.txt").toString())),
                    "Missing target cannot be declared locally available");
            // Optional caller-owned UNC, mapped-share and local-reparse-to-share
            // fixtures use the same real kernel query; no network fixture is invented.
            for (String path : nonlocalPaths) require(!(boolean)local.invoke(null, path),
                    "Nonlocal or unverifiable Windows target was accepted: " + path);
        } finally { java.nio.file.Files.deleteIfExists(file); java.nio.file.Files.deleteIfExists(directory); }
        System.out.println("DocumentPickerTest: actual Windows locality and absence queries passed");
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        fromWindowsPath();
        Class<?> host = Class.forName("com.gesher.server.WindowsHost");
        locality(host, args);
        Method pick = host.getDeclaredMethod("pickDocuments", int.class, String.class, String.class,
                String.class, String.class, String.class, boolean.class, boolean.class, int.class, BooleanSupplier.class);
        pick.setAccessible(true);
        // Repeat all modes: every COM instance, timer and hidden lifetime window
        // must retire so the next request starts and closes on its own STA thread.
        for (int repeat = 0; repeat < 2; repeat++) {
            cancelDialog(pick, 0, repeat != 0);
            cancelDialog(pick, 1, false);
            cancelDialog(pick, 2, false);
        }
        require(pick.invoke(null, 1, "Already retired fixture", "unused.txt", null, "*.txt",
                "txt", false, true, 0, (BooleanSupplier)() -> false) == null,
                "Retired owner must not open another dialog");
        System.out.println("DocumentPickerTest passed: real open/save/tree cancellation and Windows path mapping");
    }
}
