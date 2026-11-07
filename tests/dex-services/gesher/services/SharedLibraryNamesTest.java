/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.content.res.AssetManager;
import android.system.Os;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.Collection;
import java.util.Collections;
import java.util.HashSet;
import java.util.Set;
import java.util.zip.ZipOutputStream;

/** Real framework query and configuration filtering, with no app or account data. */
public final class SharedLibraryNamesTest {
    private static int checks;
    private static void require(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
        ++checks;
    }
    private static void jar(Path path) throws Exception {
        try (ZipOutputStream ignored = new ZipOutputStream(Files.newOutputStream(path))) {}
    }
    private static void xml(Path path, String body) throws Exception {
        Files.write(path, ("<permissions>" + body + "</permissions>").getBytes(StandardCharsets.UTF_8));
    }
    private static String library(String name, String file, String attributes) {
        return "<library name=\"" + name + "\" file=\"/system/framework/" + file + "\" " + attributes + "/>";
    }
    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        String original = System.getenv("ANDROID_ROOT");
        AssetManager.createSystemAssetsInZygoteLocked(false, original + "/framework/framework-res.apk");
        Path root = Files.createTempDirectory("shared-library-names-");
        Path permissions = Files.createDirectories(root.resolve("etc/permissions"));
        Path sysconfig = Files.createDirectories(root.resolve("etc/sysconfig"));
        Path framework = Files.createDirectories(root.resolve("framework"));
        try {
            Os.setenv("ANDROID_ROOT", root.toString(), true);
            Class<?> service = Class.forName("com.gesher.server.PackageManagerService");
            Object manager = service.getConstructor(Collection.class).newInstance(Collections.emptyList());
            Method query = service.getMethod("getSystemSharedLibraryNames");
            String[] empty = (String[]) query.invoke(manager);
            // The real Store collector passes this result straight to Arrays.asList.
            require(empty != null && Arrays.asList(empty).isEmpty(), "Empty system library list was null or nonempty");
            jar(framework.resolve("alpha.jar"));
            jar(framework.resolve("beta.jar"));
            xml(permissions.resolve("libraries.xml"), library("fixture.alpha", "alpha.jar", "")
                    + library("fixture.missing", "absent.jar", "")
                    + library("fixture.future", "alpha.jar", "min-device-sdk=\"9999\""));
            xml(sysconfig.resolve("libraries.xml"), library("fixture.beta", "beta.jar", ""));
            Set<String> expected = new HashSet<>(Arrays.asList("fixture.alpha", "fixture.beta"));
            String[] names = (String[]) query.invoke(manager);
            require(new HashSet<>(Arrays.asList(names)).equals(expected) && names.length == 2,
                    "Library enumeration did not match installed, SDK-applicable declarations");
            names[0] = "caller.changed";
            require(new HashSet<>(Arrays.asList((String[]) query.invoke(manager))).equals(expected),
                    "Caller mutated the registry's library snapshot");
            Files.delete(framework.resolve("beta.jar"));
            require(Arrays.equals((String[]) query.invoke(manager), new String[]{"fixture.alpha"}),
                    "Removed library file remained advertised");
            Files.write(permissions.resolve("bad.xml"), "<wrong/>".getBytes(StandardCharsets.UTF_8));
            boolean failed = false;
            try { query.invoke(manager); }
            catch (InvocationTargetException e) { failed = e.getCause() instanceof IllegalStateException; }
            require(failed, "Malformed library configuration was presented as an empty successful query");
            System.out.println("SharedLibraryNamesTest all passed: " + checks + " synthetic checks");
        } finally {
            if (original == null) Os.unsetenv("ANDROID_ROOT");
            else Os.setenv("ANDROID_ROOT", original, true);
        }
    }
}
