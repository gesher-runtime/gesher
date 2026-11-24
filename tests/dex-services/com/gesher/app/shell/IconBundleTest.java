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
            check(inputs.paths[0].equals(inputs.base)
                    && new File(inputs.base).getName().matches("[0-9]+\\.apk"),
                    "base moved first and uses a generated path");
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
    private static void zipApks(File target, String[] names, File... apks) throws IOException {
        try (ZipOutputStream out = new ZipOutputStream(Files.newOutputStream(target.toPath()))) {
            for (int i = 0; i < names.length; i++) {
                out.putNextEntry(new ZipEntry(names[i]));
                Files.copy(apks[i].toPath(), out);
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
        File base = new File(fixtures, "icon-base.apk");
        File density = new File(fixtures, "icon-density.apk");
        File nested = new File(output, "icon-bundle.APK+");
        zipApks(nested, new String[] {"apks/assets.apk", "apks/application.apk"}, density, base);
        for (String extension : new String[] {".apks", ".xapk", ".apkm", ".APK+"}) {
            String name = "icon-bundle" + extension;
            File bundle = extension.equals(".APK+") ? nested : new File(fixtures, name);
            if (!bundle.isFile() && extension.equals(".apkm")) {
                bundle = new File(output, name);
                Files.copy(new File(fixtures, "icon-bundle.apks").toPath(), bundle.toPath());
            }
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
        // framework-res.apk, Gesher's own file beside the runtime: the platform package,
        // recorded with Gesher's icon and no app package (it was "not read" before).
        File frameworkRes = new File(root + "/framework/framework-res.apk");
        check(AppShell.apkIcon(frameworkRes) == 0, "framework-res.apk is recorded");
        String frameworkKey = frameworkRes.length() + "\t" + frameworkRes.lastModified() + "\t";
        String[] frameworkRow = Files.readAllLines(new File(Device.dataDir(), "system/apk-icons.list")
                .toPath()).stream().filter(x -> x.startsWith(frameworkKey)).findFirst().orElseThrow()
                .split("\t", -1);
        check(frameworkRow.length == 4 && frameworkRow[2].endsWith("\\gesher.ico")
                && "-".equals(frameworkRow[3]), "framework-res.apk is no app: Gesher's icon");
        String log = new String(Files.readAllBytes(new File(Device.logsDir(), "apk-icons.log").toPath()),
                java.nio.charset.StandardCharsets.UTF_8);
        check(log.contains("the Android platform's resources (package android), not an app"),
                "framework-res.apk is logged as the platform's resources");
        // Its whole manifest parses: android:featureFlag elements are read through
        // android.os.flagging.AconfigPackage, which framework.jar does not contain (a
        // NoClassDefFoundError before); only then is the reserved name refused.
        try {
            ApkPackage.parse(frameworkRes.getAbsolutePath());
            throw new AssertionError("the platform package was admitted as an app");
        } catch (SecurityException expected) {
            check(String.valueOf(expected.getMessage()).contains("platform package name is reserved"),
                    "framework-res.apk's manifest parses through its feature flags");
        }
        // The flag values answered are the ones Gesher's framework runs with.
        check(android.os.flagging.AconfigPackage.load("android.content.res")
                        .getBooleanFlagValue("manifest_flagging", !android.content.res.Flags.manifestFlagging())
                        == android.content.res.Flags.manifestFlagging(),
                "aconfig flag value is the framework's own");
        check(android.os.flagging.AconfigPackage.load("android.content.res")
                        .getBooleanFlagValue("no_such_flag_in_gesher", true),
                "undeclared aconfig flag answers the caller's default");
        try {
            android.os.flagging.AconfigPackage.load("com.gesher.no.such.flag.package");
            throw new AssertionError("an aconfig package Gesher lacks was loaded");
        } catch (android.os.flagging.AconfigStorageReadException expected) {
            check(expected.getErrorCode()
                    == android.os.flagging.AconfigStorageReadException.ERROR_PACKAGE_NOT_FOUND,
                    "missing aconfig package is ERROR_PACKAGE_NOT_FOUND");
        }
        // A first launch must remember its installed identity even without a drawn icon.
        String installedPackage = ApkPackage.parse(full.getAbsolutePath()).packageName;
        check(!new File(Device.dataDir(), "data/" + installedPackage + "/gesher-shell/icon.ico").exists(),
                "fixture has no installed app icon");
        AppShell.rememberApkQuietly(installedPackage, full);
        String installedKey = full.length() + "\t" + full.lastModified() + "\t";
        String[] installedRow = Files.readAllLines(new File(Device.dataDir(), "system/apk-icons.list").toPath())
                .stream().filter(x -> x.startsWith(installedKey)).findFirst().orElseThrow().split("\t", -1);
        check(installedRow.length == 4 && installedRow[2].endsWith("\\gesher.ico")
                && installedPackage.equals(installedRow[3]), "generic icon keeps actual installed package identity");
        check(!new File(Device.dataDir(), "system/packages.list").exists(), "no package installation registry");
        check(!Device.stateFile().exists(), "no device/server started");
        File bad = new File(output, "bad.apks");
        zipApks(bad, new String[] {"base.apk", "nested/base-master.apk"}, base, base); rejects(bad);
        zip(bad, "one.apk", "two.apk"); rejects(bad);
        zipApks(bad, new String[] {"config.xxxhdpi.apk"}, density); rejects(bad);
        String[] tooMany = new String[513];
        tooMany[0] = "base.apk";
        for (int i = 1; i < tooMany.length; i++) tooMany[i] = "config." + i + ".apk";
        zip(bad, tooMany); rejects(bad);
        // Content, not the suffix, makes a bundle: these are single APK inputs.
        Files.write(bad.toPath(), new byte[] {1, 2, 3});
        try (Inputs single = Inputs.open(bad)) {
            check(single.paths.length == 1 && single.base.equals(bad.getAbsolutePath()),
                    "a file that is not a ZIP is read as an APK, not a bundle");
        }
        File plainXapk = new File(output, "plain.xapk");
        Files.copy(full.toPath(), plainXapk.toPath());
        try (Inputs single = Inputs.open(plainXapk)) {
            check(single.paths.length == 1 && single.base.equals(plainXapk.getAbsolutePath()),
                    "an APK saved as .xapk is read as the APK it is");
        }
        check(color(ico(plainXapk, new File(output, "plain.xapk.ico"))) == red, "APK named .xapk draws its icon");
        File bundleApk = new File(output, "icon-bundle-saved-as.apk");
        Files.copy(new File(fixtures, "icon-bundle.xapk").toPath(), bundleApk.toPath());
        check(color(bundleIco(bundleApk, new File(output, "bundle-as-apk.ico"))) == red,
                "a bundle saved as .apk is read as the bundle it is");
        File temporary;
        try (Inputs inputs = Inputs.open(nested)) {
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
