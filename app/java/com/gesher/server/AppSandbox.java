/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import java.io.File;
import java.io.IOException;
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

    // Windows file ACL masks. Modify excludes WRITE_DAC and WRITE_OWNER.
    static final int READ = 0x00120089;
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
    static int spawn(String commandLine, List<String> env, File logFile,
            PackageManagerService pm, ApkPackage pkg, String packageName, boolean internet) {
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
        // The shared boot property snapshots are read-only to apps. Persistent
        // properties and the rest of the system data remain outside the grant.
        grantCapability(new File(Device.dataDir(), "system/gesher-properties").getPath(),
                RUNTIME_FILES, READ, true);
        // The user's CA certificates, which every app reads as on Android (KeyChain makes
        // them world-readable): $ANDROID_DATA/misc/user/0 (docs/network.md).
        File userCas = new File(Device.dataDir(), "misc/user/0/cacerts-added");
        userCas.mkdirs();
        grantCapability(userCas.getParent(), RUNTIME_FILES, READ_EXECUTE, true);
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
        // Publication prepares the replacement file's ACL and reconciles live
        // storage grants through the same path as permission/AppOp changes.
        ExternalStorageAccess.syncPackage(pm, pkg);
        List<String> caps = capabilities(internet);
        SystemServer.log("sandbox: " + packageName + " in AppContainer " + container
                + " with " + caps);
        return Zygote.spawnInAppContainer(commandLine, env, logFile.getPath(), container,
                packageName, caps);
    }

    /**
     * Prepare the new inode before atomic policy publication: replacing a file
     * does not copy its old explicit AppContainer ACE to the replacement.
     */
    static void prepareStoragePolicyFile(String packageName, File stagedFile) throws IOException {
        if (!enabled()) return;
        File policyRoot = new File(Device.dataDir(), "system/storage-policy").getCanonicalFile();
        File staged = stagedFile.getCanonicalFile();
        if (!policyRoot.equals(staged.getParentFile()) || !staged.isFile()) {
            throw new IOException("Storage policy file outside the policy directory: " + stagedFile);
        }
        setStorageAccess(staged, containerName(packageName), READ_EXECUTE, false);
    }

    /**
     * Reconcile one immutable policy for a running package as well as at spawn.
     * The publisher serializes this with policy publication; failures propagate
     * so a requested permission change is never reported as successfully applied.
     * A null policy removes an uninstalled package's grants without creating paths.
     */
    static void syncStorageAccess(String packageName, ExternalStorageAccess.Policy policy)
            throws IOException {
        if (!enabled()) return;
        if (packageName == null || packageName.isEmpty() || packageName.indexOf('/') >= 0
                || packageName.indexOf('\\') >= 0 || ".".equals(packageName)
                || "..".equals(packageName)) {
            throw new IOException("Invalid storage package name: " + packageName);
        }
        String container = containerName(packageName);
        File sharedRoot = new File(Device.dataDir(), "media/0").getCanonicalFile();
        boolean removed = policy == null;
        if (removed && !sharedRoot.exists()) return;
        requireDirectory(sharedRoot);
        File android = storageDirectory(sharedRoot, "Android", !removed);
        // Protect these roots before making the shared-root ACE inheritable.
        // Protection retains Windows owner/SYSTEM access and explicit grants on
        // each package's own directory; it does not add a deny ACE to all apps.
        for (String sub : new String[] {"data", "obb"}) {
            File privateRoot = storageDirectory(android, sub, !removed);
            if (removed && !privateRoot.exists()) continue;
            int error = Zygote.nativeProtectPrivateStorage(Zygote.windowsPath(privateRoot.getPath()));
            if (error != 0) throw storageFailure("protecting", privateRoot, error);
        }
        File media = storageDirectory(android, "media", !removed);
        int sharedAccess = removed ? 0 : switch (policy.mode()) {
            case MANAGER, LEGACY_RW -> MODIFY;
            case LEGACY_RO, MEDIA_RO -> READ_EXECUTE;
            case SCOPED -> 0;
        };
        setStorageAccess(sharedRoot, container, sharedAccess, true);

        // Migrate the earlier non-inheriting-root layout. Explicit child grants
        // otherwise survive a root downgrade/revoke and continue to allow writes.
        File[] children = sharedRoot.listFiles();
        if (children == null) throw new IOException("Cannot enumerate shared storage: " + sharedRoot);
        for (File child : children) {
            File resolved = child.getCanonicalFile();
            if (!sharedRoot.equals(resolved.getParentFile())
                    || !child.getName().equals(resolved.getName())) {
                throw new IOException("Reparse child in shared storage: " + child);
            }
            setStorageAccess(child, container, 0, false);
        }
        if (media.exists()) setStorageAccess(media, container, 0, false);
        for (String sub : new String[] {"data", "obb", "media"}) {
            File appDir = storageDirectory(new File(android, sub), packageName, !removed);
            if (appDir.exists()) setStorageAccess(appDir, container, removed ? 0 : MODIFY, true);
        }
    }

    private static File storageDirectory(File parent, String name, boolean create) throws IOException {
        File child = new File(parent, name);
        File resolved = child.getCanonicalFile();
        if (!parent.equals(resolved.getParentFile()) || !name.equals(resolved.getName())) {
            throw new IOException("Storage directory outside its parent: " + child);
        }
        if (create) requireDirectory(child);
        return child;
    }

    private static void requireDirectory(File directory) throws IOException {
        if (!directory.isDirectory() && !directory.mkdirs() && !directory.isDirectory()) {
            throw new IOException("Cannot create storage directory: " + directory);
        }
    }

    private static void setStorageAccess(File path, String container, int access, boolean inherit)
            throws IOException {
        int error = Zygote.nativeSetAppContainerStorageAccess(
                Zygote.windowsPath(path.getPath()), container, access, inherit);
        if (error != 0) throw storageFailure("setting permissions on", path, error);
    }

    private static IOException storageFailure(String operation, File path, int error) {
        return new IOException("AppContainer storage: " + operation + " " + path
                + ": Win32 error " + error);
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
