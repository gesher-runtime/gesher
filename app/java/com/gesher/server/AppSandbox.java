/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

/**
 * Experimental per-package AppContainer isolation of app processes (ADR 0090,
 * docs/sandbox.md), on only with {@code GESHER_SANDBOX=appcontainer}.
 *
 * <p>The system server is the broker: before it starts an app process it creates the
 * package's AppContainer profile, opens the runtime's files to the {@code
 * gesherRuntimeFiles} capability (read and execute), opens the package's own code,
 * data, profile and log files to the package's container SID, and starts the process
 * with a token of that container and only the capabilities its permissions map to.
 * Everything else the Windows user can reach stays closed to the process, as the kernel
 * enforces it. This milestone (M1) is a proof of concept: docs/sandbox.md lists what
 * works, what does not yet, and why it is not a security boundary yet.
 */
final class AppSandbox {
    private AppSandbox() {
    }

    /** The environment switch, as binderd reads it too. */
    static final String ENV = "GESHER_SANDBOX";
    static final String RUNTIME_FILES = "gesherRuntimeFiles";
    static final String BINDER_CLIENT = "gesherBinderClient";
    static final String INTERNET_CLIENT = "internetClient";
    static final String PRIVATE_NETWORK = "privateNetworkClientServer";

    // FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, and Windows' "Modify" (no WRITE_DAC/OWNER).
    static final int READ_EXECUTE = 0x001200A9;
    static final int MODIFY = 0x001301BF;

    static boolean enabled() {
        return SystemServer.isProtectedDevice() || "appcontainer".equalsIgnoreCase(System.getenv(ENV));
    }

    /**
     * The package's profile name: CreateAppContainerProfile takes at most 64 characters
     * of a restricted set, so the device's data root and the package name are hashed, as
     * Chromium names its own. The root keeps two devices of one Windows user (another
     * data root) from sharing a container, its grants and its deletion at uninstall.
     */
    static String containerName(String packageName) {
        return containerName(canonical(new File(Device.dataDir())).getPath(), packageName);
    }

    static String installationContainer(ApkPackage pkg) {
        if (!enabled()) return null;
        if (!SystemServer.isProtectedDevice()) return containerName(pkg.packageName);
        String owner = java.util.UUID.fromString(pkg.enabledStateOwner()).toString();
        return containerName(pkg.packageName + "\0" + owner);
    }

    /**
     * The canonical form, as the installer builds code directories (a {@code /mnt/<drive>}
     * path or an 8.3 short name names the same directory); the absolute one if the
     * canonical cannot be read.
     */
    static File canonical(File file) {
        try {
            return file.getCanonicalFile();
        } catch (java.io.IOException e) {
            return file.getAbsoluteFile();
        }
    }

    static String containerName(String dataRoot, String packageName) {
        try {
            byte[] digest = MessageDigest.getInstance("SHA-256")
                    .digest((dataRoot + "\0" + packageName).getBytes(StandardCharsets.UTF_8));
            StringBuilder sb = new StringBuilder("Gesher.App.");
            for (int i = 0; i < 20; i++) sb.append(String.format("%02x", digest[i] & 0xff));
            return sb.toString();
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException("SHA-256 unavailable", e);
        }
    }

    /**
     * The capabilities of the package's token. INTERNET is Android's whole network
     * access, the Internet and the local network both; nothing else maps to a
     * capability in this milestone (devices stay with the system server's services).
     */
    static List<String> capabilities(boolean internet) {
        List<String> caps = new ArrayList<>(List.of(RUNTIME_FILES, BINDER_CLIENT));
        if (internet) {
            caps.add(INTERNET_CLIENT);
            caps.add(PRIVATE_NETWORK);
        }
        return caps;
    }

    /**
     * Prepares the package's container and starts {@code commandLine} in it. Grants are
     * idempotent: an ACE already there is not written (or propagated) again.
     */
    static int spawn(String commandLine, List<String> env, File logFile, ApkPackage pkg,
            String packageName, boolean internet, long startSeq, Zygote.StandaloneInput input) {
        String container = installationContainer(pkg);
        int error = Zygote.nativeEnsureAppContainer(container, packageName);
        if (error != 0) {
            throw new IllegalStateException("AppContainer profile " + container + " for "
                    + packageName + ": Win32 error " + error);
        }
        int propertyError = Zygote.nativeGrantPropertyAreaRead(container);
        if (propertyError != 0) {
            throw new IllegalStateException("AppContainer property area for " + packageName
                    + ": Win32 error " + propertyError);
        }
        if (SystemServer.isProtectedDevice()) {
            prepareStandaloneRuntime();
        } else {
            // The earlier developer fixture keeps its existing layout.
            String exeDir = exeDirectory();
            if (exeDir != null) {
                checkGrant(Zygote.nativeGrantToCapability(exeDir, RUNTIME_FILES, READ_EXECUTE, true),
                        RUNTIME_FILES, exeDir);
            }
            for (String dir : runtimeDirectories()) {
                grantCapability(dir, RUNTIME_FILES, READ_EXECUTE, true);
            }
        }
        // The APK itself; its directory (and the oat files in it) only when it is the
        // package's own code directory under data/app. An APK run from where the user
        // keeps it (Downloads) must not open that directory to the container.
        if (input == null) {
            File apk = grantPath(new File(pkg.apkPath));
            grantContainer(apk.getPath(), container, READ_EXECUTE, false);
            File apps = grantPath(new File(Device.dataDir(), "app"));
            File code = apk.getParentFile();
            if (code != null && apps.getPath().equals(code.getParent())) {
                grantContainer(code.getPath(), container, READ_EXECUTE, true);
            }
        }
        grantContainer(pkg.credentialProtectedDataDir(), container, MODIFY, true);
        grantContainer(pkg.deviceProtectedDataDir(), container, MODIFY, true);
        File profile = Dexopt.currentProfile(packageName).getParentFile();
        if (profile != null) grantContainer(profile.getPath(), container, MODIFY, true);
        // The logcat file the process opens by name (GESHER_LOG_FILE); its stdout and
        // stderr are an inherited handle and need no grant.
        File logcat = new File(logFile.getPath() + ".logcat");
        try {
            logcat.getParentFile().mkdirs();
            logcat.createNewFile();
        } catch (java.io.IOException e) {
            if (SystemServer.isProtectedDevice()) {
                throw new IllegalStateException("Cannot create contained log " + logcat, e);
            }
            SystemServer.log("sandbox: cannot create " + logcat + ": " + e);
        }
        grantContainer(logcat.getPath(), container, MODIFY, false);
        List<String> caps = capabilities(internet);
        SystemServer.log("sandbox: " + packageName + " in AppContainer " + container
                + " with " + caps);
        if (SystemServer.isProtectedDevice()) {
            return Zygote.spawnProtectedApp(commandLine, env, logFile.getPath(), container,
                    caps, pkg, startSeq, input);
        }
        return Zygote.spawnInAppContainer(commandLine, env, logFile.getPath(), container,
                packageName, caps);
    }

    /** The package is uninstalled: its container's profile goes with it. */
    static void deleteContainer(String packageName, String container) {
        if (container == null) return;
        int error = Zygote.nativeDeleteAppContainer(container);
        if (error != 0) {
            SystemServer.log("sandbox: deleting AppContainer " + container + " of "
                    + packageName + ": Win32 error " + error);
        }
    }

    /** dalvikvm.exe's directory as a Windows path, or null. */
    static String exeDirectory() {
        String exe = Zygote.exePath();
        int slash = exe == null ? -1 : exe.lastIndexOf('\\');
        return slash > 0 ? exe.substring(0, slash) : null;
    }

    /**
     * What every app process reads and runs besides dalvikvm.exe's directory: the
     * Android roots the device points it at and the boot class path's directories.
     */
    static Set<String> runtimeDirectories() {
        Set<String> dirs = new LinkedHashSet<>();
        for (String name : new String[] {"ANDROID_ROOT", "ANDROID_ART_ROOT", "ANDROID_I18N_ROOT",
                "ANDROID_TZDATA_ROOT"}) {
            String value = System.getenv(name);
            if (value != null && !value.isEmpty()) dirs.add(value);
        }
        String boot = System.getProperty("java.boot.class.path", "");
        for (String entry : boot.split(":")) {
            if (entry.isEmpty()) continue;
            File parent = new File(entry).getParentFile();
            if (parent != null) dirs.add(parent.getPath());
        }
        return dirs;
    }

    static void prepareStandaloneRuntime() {
        String exeDir = exeDirectory();
        if (exeDir == null) throw new IllegalStateException("The contained runtime is unavailable");
        String runtime = Zygote.fromWindowsPath(exeDir);
        // Share the runtime files, not other files that happen to sit next to
        // dalvikvm (including a standalone APK or DEX chosen by the caller).
        ensureGrant(Zygote.nativeGrantToCapability(exeDir, RUNTIME_FILES, READ_EXECUTE, false),
                RUNTIME_FILES, exeDir);
        for (String name : List.of("dalvikvm.exe", "gesher-exec.exe", "gesher_arm64.dll",
                "libEGL.dll", "libGLESv2.dll", "WebView2Loader.dll", "libgesher_webview_bridge.so",
                "framework-res.apk", "build.prop")) {
            grantRuntimePath(new File(runtime, name), false);
        }
        grantRuntimePath(new File(runtime, "framework"), true);
        for (String name : List.of("ANDROID_ROOT", "ANDROID_ART_ROOT", "ANDROID_I18N_ROOT",
                "ANDROID_TZDATA_ROOT")) {
            String directory = System.getenv(name);
            if (directory != null && !directory.isEmpty()) grantRuntimePath(new File(directory), true);
        }
        for (String jar : System.getProperty("java.boot.class.path", "").split(":")) {
            if (!jar.isEmpty()) grantRuntimePath(new File(jar), false);
        }
    }

    private static void grantRuntimePath(File path, boolean inherit) {
        if (!path.exists()) return;
        String windows = Zygote.windowsPath(path.getPath());
        ensureGrant(Zygote.nativeGrantToCapability(windows, RUNTIME_FILES, READ_EXECUTE, inherit),
                RUNTIME_FILES, windows);
    }

    private static void grantCapability(String path, String capability, int access,
            boolean inherit) {
        if (path == null || !new File(path).exists()) return;
        String target = Zygote.windowsPath(path);
        int error = Zygote.nativeGrantToCapability(target, capability, access, inherit);
        checkGrant(error, capability, target);
    }

    private static void grantContainer(String path, String container, int access,
            boolean inherit) {
        if (path == null || !new File(path).exists()) return;
        String target = Zygote.windowsPath(path);
        int error = Zygote.nativeGrantToAppContainer(target, container, access, inherit);
        checkGrant(error, container, target);
    }

    private static File grantPath(File path) {
        // Keep the requested spelling for the native handle check. Resolving a
        // guest-controlled junction here could hide where the path was redirected.
        return SystemServer.isProtectedDevice() ? path.getAbsoluteFile() : canonical(path);
    }

    private static void checkGrant(int error, String sid, String target) {
        if (error == 0) return;
        String message = "sandbox: granting " + sid + " " + target + ": Win32 error " + error;
        if (SystemServer.isProtectedDevice()) throw new IllegalStateException(message);
        SystemServer.log(message);
    }

    private static void ensureGrant(int error, String sid, String target) {
        if (error != 0) throw new IllegalStateException("Could not grant " + sid + " access to "
                + target + " (Win32 error " + error + ")");
    }
}
