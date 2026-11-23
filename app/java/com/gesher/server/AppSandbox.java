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
        return "appcontainer".equalsIgnoreCase(System.getenv(ENV));
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
            String packageName, boolean internet) {
        String container = containerName(packageName);
        int error = Zygote.nativeEnsureAppContainer(container, packageName);
        if (error != 0) {
            throw new IllegalStateException("AppContainer profile " + container + " for "
                    + packageName + ": Win32 error " + error);
        }
        // dalvikvm.exe's own directory: a Windows path already (GetModuleFileNameW).
        String exeDir = exeDirectory();
        if (exeDir != null) {
            int granted = Zygote.nativeGrantToCapability(exeDir, RUNTIME_FILES, READ_EXECUTE, true);
            if (granted != 0) {
                SystemServer.log("sandbox: granting " + RUNTIME_FILES + " " + exeDir
                        + ": Win32 error " + granted);
            }
        }
        for (String dir : runtimeDirectories()) {
            grantCapability(dir, RUNTIME_FILES, READ_EXECUTE, true);
        }
        // The APK itself; its directory (and the oat files in it) only when it is the
        // package's own code directory under data/app. An APK run from where the user
        // keeps it (Downloads) must not open that directory to the container.
        File apk = canonical(new File(pkg.apkPath));
        grantContainer(apk.getPath(), container, READ_EXECUTE, false);
        File apps = canonical(new File(Device.dataDir(), "app"));
        File code = apk.getParentFile();
        if (code != null && apps.getPath().equals(code.getParent())) {
            grantContainer(code.getPath(), container, READ_EXECUTE, true);
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
            SystemServer.log("sandbox: cannot create " + logcat + ": " + e);
        }
        grantContainer(logcat.getPath(), container, MODIFY, false);
        List<String> caps = capabilities(internet);
        SystemServer.log("sandbox: " + packageName + " in AppContainer " + container
                + " with " + caps);
        return Zygote.spawnInAppContainer(commandLine, env, logFile.getPath(), container,
                packageName, caps);
    }

    /** The package is uninstalled: its container's profile goes with it. */
    static void deleteContainer(String packageName) {
        if (!enabled()) return;
        String container = containerName(packageName);
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

    private static void grantCapability(String path, String capability, int access,
            boolean inherit) {
        if (path == null || !new File(path).exists()) return;
        String target = Zygote.windowsPath(path);
        int error = Zygote.nativeGrantToCapability(target, capability, access, inherit);
        if (error != 0) {
            SystemServer.log("sandbox: granting " + capability + " " + target
                    + ": Win32 error " + error);
        }
    }

    private static void grantContainer(String path, String container, int access,
            boolean inherit) {
        if (path == null || !new File(path).exists()) return;
        String target = Zygote.windowsPath(path);
        int error = Zygote.nativeGrantToAppContainer(target, container, access, inherit);
        if (error != 0) {
            SystemServer.log("sandbox: granting " + container + " " + target
                    + ": Win32 error " + error);
        }
    }
}
