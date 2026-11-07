/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.app.shell;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import com.gesher.app.Main;
import com.gesher.server.ApkPackage;
import com.gesher.server.Device;
import java.io.File;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.util.List;
import java.util.concurrent.atomic.AtomicReference;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/** Real resources/graphics and filesystem tests; no app code, server, install or user UI. */
public final class IconBundleTest {
    private static int checks;
    // Product classes are on the boot loader while fixture classes use the app
    // loader. Reflect the package-private helper without changing its API.
    private static final class Inputs implements AutoCloseable {
        final AutoCloseable delegate;
        final String base;
        final String[] paths;
        Inputs(Object value) throws Exception {
            delegate = (AutoCloseable)value;
            var b = value.getClass().getDeclaredField("base"); b.setAccessible(true);
            var p = value.getClass().getDeclaredField("paths"); p.setAccessible(true);
            base = (String)b.get(value); paths = (String[])p.get(value);
        }
        static Inputs open(File source) throws Exception {
            var open = Class.forName("com.gesher.app.shell.IconApks").getDeclaredMethod("open", File.class);
            open.setAccessible(true);
            try { return new Inputs(open.invoke(null, source)); }
            catch (java.lang.reflect.InvocationTargetException failure) {
                if (failure.getCause() instanceof Exception) throw (Exception)failure.getCause();
                throw new AssertionError(failure.getCause());
            }
        }
        @Override public void close() throws Exception { delegate.close(); }
    }
    private static void check(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
        checks++;
    }
    private static int color(File ico) throws Exception {
        byte[] bytes = Files.readAllBytes(ico.toPath());
        ByteBuffer b = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);
        check(b.getShort(0) == 0 && b.getShort(2) == 1, "ICO header");
        int count = Short.toUnsignedInt(b.getShort(4));
        check(count == AppIcon.SIZES.length, "all icon sizes retained");
        for (int i = 0; i < count; i++) {
            int entry = 6 + 16 * i;
            int size = b.getInt(entry + 8), offset = b.getInt(entry + 12);
            check(size > 0 && offset >= 6 + 16 * count && offset <= bytes.length - size,
                    "complete image inside ICO");
            if (bytes[entry] != 0) continue;
            Bitmap bitmap = BitmapFactory.decodeByteArray(bytes, offset, size);
            check(bitmap != null && bitmap.getWidth() == 256 && bitmap.getHeight() == 256,
                    "real 256px PNG decodes after resource closure");
            int center = bitmap.getPixel(128, 128);
            check((bitmap.getPixel(2, 2) >>> 24) == 0, "transparent legacy icon corners");
            bitmap.recycle();
            return center;
        }
        throw new AssertionError("No 256px ICO image");
    }
    private static File ico(File apk, File output) throws Exception {
        ApkPackage p = ApkPackage.parse(apk.getAbsolutePath());
        AppIcon.load(apk.getAbsolutePath(), p.applicationInfo(0), null).writeIco(output);
        return output;
    }
    private static File bundleIco(File bundle, File output) throws Exception {
        File temp;
        try (Inputs inputs = Inputs.open(bundle)) {
            temp = new File(inputs.base).getParentFile();
            check(inputs.paths.length == 2, "base plus density APK");
            check(new File(inputs.base).getName().equals("0.apk"), "base moved first and generated path");
            ApkPackage p = ApkPackage.parse(inputs.base);
            AppIcon icon = AppIcon.load(inputs.paths, p.applicationInfo(0), null);
            check(icon.source.contains("xxxhdpi"), "best-density resource from split");
            icon.writeIco(output);
        }
        check(!temp.exists(), "extracted APK directory retired on close");
        return output;
    }
    private static void zip(File target, String... names) throws Exception {
        try (ZipOutputStream out = new ZipOutputStream(Files.newOutputStream(target.toPath()))) {
            for (String name : names) {
                out.putNextEntry(new ZipEntry(name));
                out.write(7);
                out.closeEntry();
            }
        }
    }
    private static void rejects(File file) throws Exception {
        try (Inputs ignored = Inputs.open(file)) {
            throw new AssertionError("Bad bundle accepted: " + file.getName());
        } catch (IOException expected) { checks++; }
    }
    public static void main(String[] args) throws Exception {
        if (args.length != 3) throw new IllegalArgumentException("Pass fixture dir, full PNG APK, output dir");
        String root = System.getenv("ANDROID_ROOT");
        Main.bootstrap("IconBundleTest", root + "/build.prop", root + "/framework/framework-res.apk");
        File fixtures = new File(args[0]), full = new File(args[1]), output = new File(args[2]);
        Files.createDirectories(output.toPath());
        File tempRoot = new File(System.getProperty("java.io.tmpdir"));
        File[] startingTemps = tempRoot.listFiles((dir, name) -> name.startsWith("gesher-icon-"));
        check(startingTemps != null, "isolated Java temp root exists");
        int red = 0xffd32f2f;
        check(color(ico(full, new File(output, "ordinary.ico"))) == red, "ordinary APK icon preserved");
        check(color(ico(new File(fixtures, "icon-base.apk"), new File(output, "base-only.ico"))) != red,
                "causal control: base alone lacks xxxhdpi red asset");
        for (String name : new String[] {"icon-bundle.apks", "icon-bundle.xapk"}) {
            File bundle = new File(fixtures, name);
            check(color(bundleIco(bundle, new File(output, name + ".ico"))) == red,
                    "bundle split pixels match the ordinary APK");
            check(AppShell.apkIcon(bundle) == 0, "real icon command reads bundle");
            List<String> lines = Files.readAllLines(new File(Device.dataDir(), "system/apk-icons.list").toPath());
            String key = bundle.length() + "\t" + bundle.lastModified() + "\t";
            String row = lines.stream().filter(x -> x.startsWith(key)).findFirst().orElseThrow();
            String iconPath = row.split("\t", -1)[2];
            check(!iconPath.endsWith("gesher.ico"), "cache is drawn app icon, not fallback");
            String leaf = iconPath.replace('\\', '/');
            leaf = leaf.substring(leaf.lastIndexOf('/') + 1);
            check(color(new File(Device.dataDir(), "system/apk-icons/" + leaf)) == red, "published cached pixels");
        }
        check(!new File(Device.dataDir(), "system/packages.list").exists(), "no package installation registry");
        check(!Device.stateFile().exists(), "no device/server started");
        File bad = new File(output, "bad.apks");
        zip(bad, "base.apk", "nested/base-master.apk"); rejects(bad);
        zip(bad, "one.apk", "two.apk"); rejects(bad);
        zip(bad, "config.xxxhdpi.apk"); rejects(bad);
        String[] tooMany = new String[513];
        tooMany[0] = "base.apk";
        for (int i = 1; i < tooMany.length; i++) tooMany[i] = "config." + i + ".apk";
        zip(bad, tooMany); rejects(bad);
        Files.write(bad.toPath(), new byte[] {1, 2, 3}); rejects(bad);
        zip(bad, "../../base.apk", "../config.xxxhdpi.apk");
        File temporary;
        try (Inputs inputs = Inputs.open(bad)) {
            temporary = new File(inputs.base).getParentFile();
            for (String path : inputs.paths) {
                check(new File(path).getParentFile().equals(temporary), "archive paths cannot escape generated directory");
            }
        }
        check(!temporary.exists(), "path-control extraction cleaned");
        File[] endingTemps = tempRoot.listFiles((dir, name) -> name.startsWith("gesher-icon-"));
        check(endingTemps != null && endingTemps.length == startingTemps.length,
                "successful and rejected bundle temporary directories cleaned");
        ApkPackage p = ApkPackage.parse(full.getAbsolutePath());
        AppIcon shared = AppIcon.load(full.getAbsolutePath(), p.applicationInfo(0), null);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        Thread[] writers = new Thread[8];
        File concurrent = new File(output, "concurrent.ico");
        for (int i = 0; i < writers.length; i++) {
            writers[i] = new Thread(() -> {
                try { shared.writeIco(concurrent); } catch (Throwable t) { failure.compareAndSet(null, t); }
            });
            writers[i].start();
        }
        for (Thread thread : writers) thread.join(15000);
        for (Thread thread : writers) check(!thread.isAlive(), "concurrent icon writer completed");
        if (failure.get() != null) throw new AssertionError("Concurrent ICO publication failed", failure.get());
        check(color(concurrent) == red, "concurrent complete ICO publication");
        File[] leftovers = output.listFiles((dir, name) -> name.startsWith("icon-") && name.endsWith(".tmp"));
        check(leftovers != null && leftovers.length == 0, "unique temporary ICOs cleaned");
        System.out.println("IconBundleTest all passed: " + checks + " checks");
    }
}
