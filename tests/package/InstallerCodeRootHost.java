/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
import java.io.File;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.List;

/** Actual Installer/ApkPackage filesystem boundaries; no Android parsing or I/O adapters. */
public final class InstallerCodeRootHost {
    private static int checks;
    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        ++checks;
    }
    private static Method method(Class<?> type, String name, Class<?>... parameters) throws Exception {
        Method method = type.getDeclaredMethod(name, parameters);
        method.setAccessible(true);
        return method;
    }
    private static void rejected(Method method, String message, Object... arguments) throws Exception {
        try {
            method.invoke(null, arguments);
            throw new AssertionError(message);
        } catch (InvocationTargetException failure) {
            require(failure.getCause() instanceof SecurityException,
                    message + ": wrong failure " + failure.getCause());
        }
    }
    public static void main(String[] args) throws Exception {
        File configured = new File(System.getenv("ANDROID_DATA")).getAbsoluteFile();
        File physical = configured.getCanonicalFile();
        require(!configured.equals(physical), "Host fixture needs a real noncanonical root spelling");
        Path root = physical.toPath();
        Path app = root.resolve("app");
        Class<?> installer = Class.forName("com.gesher.server.Installer");
        Method load = method(installer, "loadInstalled");
        byte[] marker = new byte[]{7, 19, 41};
        Files.write(app, marker);
        rejected(load, "Installer accepted a file as its APK root");
        require(Arrays.equals(marker, Files.readAllBytes(app)), "Rejected root file was changed");
        Files.delete(app);

        Path outside = Files.createDirectory(root.resolve("outside"));
        Path sentinel = outside.resolve("keep");
        Files.write(sentinel, marker);
        Files.createSymbolicLink(app, outside);
        rejected(load, "Installer followed an app-root symlink during registry scan");
        require(Files.isSymbolicLink(app) && Arrays.equals(marker, Files.readAllBytes(sentinel)),
                "Rejected app-root symlink or target was modified");
        rejected(method(installer, "codeRoot"), "Installer accepted an app-root symlink for staging");
        Files.delete(app);

        Method codeRoot = method(installer, "codeRoot");
        require(app.toFile().equals(codeRoot.invoke(null)), "Configured root alias leaked into code paths");
        Files.createDirectory(app);
        require(app.toFile().equals(codeRoot.invoke(null)), "Existing code root changed its physical namespace");
        require(((List<?>) load.invoke(null)).isEmpty(), "Empty real code root invented installed packages");

        Method parse = method(Class.forName("com.gesher.server.ApkPackage"),
                "parseCodeDirectory", String.class);
        rejected(parse, "Parser accepted an external directory", outside.toString());
        Path linkedCode = app.resolve("fixture.package-1");
        Files.createSymbolicLink(linkedCode, outside);
        rejected(parse, "Parser followed an installed-generation symlink", linkedCode.toString());
        require(Files.isSymbolicLink(linkedCode)
                && Arrays.equals(marker, Files.readAllBytes(sentinel)),
                "Rejected generation symlink or target was modified");
        System.out.println("InstallerCodeRootHost all passed: " + checks + " checks");
    }
}
