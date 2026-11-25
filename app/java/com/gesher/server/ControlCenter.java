/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.Locale;
import java.util.UUID;

/**
 * Opens Gesher's Control Center (tools/package/gesher-control-panel.ps1), optionally on one
 * app's details: Android's "App info", which Gesher has no Settings APK for
 * ({@link SettingsIntents}).
 *
 * <p>Each request is an immutable file in {@link #REQUESTS}, containing the package
 * and its optional {@link SettingsResults} token together. The Control Center claims a
 * file by renaming it, so publication, ownership and result routing cannot mix requests.
 * ShellExecute opens the installed panel shortcut; a second panel start signals the
 * existing panel, which drains the queue on its live-refresh timer.
 */
final class ControlCenter {
    private ControlCenter() {
    }

    static final String REQUEST = "system/control-panel-request";
    static final String SHORTCUT = "system/gesher-control-panel.lnk";
    /** Immutable request records; the host atomically renames each one to claim it. */
    static final String REQUESTS = "system/control-panel-requests";
    static final int MAX_QUEUED = 32;
    private static long sLastRequestOrder;
    /** Legacy paths are cleaned at a device restart; current publication does not use them. */
    static final String RESULTS = "system/control-panel-results";
    static final String SHOWING = "system/control-panel-showing";

    /** Opens the Control Center for a request awaiting a result. */
    interface Opener {
        /** The cancellation hook is retained for server-local launch fixtures. */
        boolean open(String packageName, String token, java.util.function.Consumer<String> replaced);
    }

    private static final java.util.function.Predicate<String> HOST_OPEN =
            pkg -> openOnHost(pkg, null, null);
    /** Replaced by tests: opens the Control Center on a package (null: its main view). */
    static volatile java.util.function.Predicate<String> sOpen = HOST_OPEN;
    /** Replaced by tests: opens it on a package for a request awaiting its result token. */
    static volatile Opener sOpenForResult = ControlCenter::openOnHost;

    static boolean open(String packageName) {
        return open(packageName, null, null);
    }

    /** With a token (SettingsResults), the Control Center reports when the details close. */
    static boolean open(String packageName, String token,
            java.util.function.Consumer<String> replaced) {
        try {
            if (token == null) {
                java.util.function.Predicate<String> open = sOpen;
                return open == HOST_OPEN ? openOnHost(packageName, null, replaced)
                        : open.test(packageName);
            }
            if (packageName == null || !validToken(token)) return false;
            return sOpenForResult.open(packageName, token, replaced);
        } catch (RuntimeException | LinkageError e) {
            SystemServer.log("Control Center: " + e);
            return false;
        }
    }

    /** SettingsResults.validToken, kept here: the host request test builds this class alone. */
    static boolean validToken(String token) {
        return token != null && token.matches("[0-9a-f]{32}");
    }

    private static synchronized boolean openOnHost(String packageName, String token,
            java.util.function.Consumer<String> replaced) {
        File request = new File(Device.dataDir(), REQUEST);
        String exe = Zygote.exePath();
        if (exe == null || exe.lastIndexOf('\\') < 0) return false;
        String launcher = exe.substring(0, exe.lastIndexOf('\\')) + "\\gesher-launch.exe";
        if (!com.gesher.app.shell.Shell.exists(launcher)) {
            SystemServer.log("Control Center: no " + launcher + " (not an installed Gesher)");
            return false;
        }
        File directory = request.getParentFile();
        if (!directory.isDirectory() && !directory.mkdirs() && !directory.isDirectory()) return false;
        String link = WindowsHost.fullPath(new File(Device.dataDir(), SHORTCUT).getPath());
        String dir = launcher.substring(0, launcher.lastIndexOf('\\'));
        int hr = com.gesher.app.shell.Shell.createShortcut(link, launcher, "control-panel", dir,
                dir + "\\gesher.ico", "Gesher", "Gesher.ControlPanel");
        if (hr != 0 && !com.gesher.app.shell.Shell.exists(link)) {
            SystemServer.log("Control Center: no shortcut: 0x" + Integer.toHexString(hr));
            return false;
        }
        final File published;
        try {
            published = publishRequest(new File(Device.dataDir(), REQUESTS), packageName, token);
        } catch (IOException e) {
            SystemServer.log("Control Center: no request file: " + e);
            return false;
        }
        boolean opened = false;
        try {
            opened = WindowsHost.openUrl(link);
            return opened;
        } finally {
            // A failed launch owns only this immutable record, never another request.
            if (!opened) published.delete();
        }
    }

    /**
     * Atomically publishes the complete request. No later request replaces a queued or
     * already claimed visit, even for another package. The filename gives FIFO ordering
     * and a unique ownership identity; the token travels inside the same atomic record.
     */
    static synchronized File publishRequest(File requests, String packageName, String token)
            throws IOException {
        if (packageName != null && (!packageName.matches("[A-Za-z][A-Za-z0-9_]*(\\.[A-Za-z][A-Za-z0-9_]*)+")
                || packageName.length() > 255)) throw new IOException("invalid package");
        if (token != null && (packageName == null || !validToken(token))) {
            throw new IOException("invalid token");
        }
        File[] waiting = requests.listFiles((directory, name) -> name.endsWith(".request"));
        if (waiting != null && waiting.length >= MAX_QUEUED) {
            throw new IOException("Control Center request queue is full");
        }
        long order = Math.max(System.currentTimeMillis(), sLastRequestOrder + 1);
        sLastRequestOrder = order;
        String identity = token != null ? token : "host-" + UUID.randomUUID().toString().replace("-", "");
        File record = new File(requests, String.format(Locale.ROOT, "%019d-%s.request", order, identity));
        writeAtomically(record, (packageName == null ? "" : packageName) + "\n"
                + (token == null ? "" : token));
        return record;
    }

    /** A new device has no callers waiting from an earlier system_server. */
    static void clearResults(String dataDir) {
        for (String directory : new String[]{REQUESTS, RESULTS}) {
            File[] files = new File(dataDir, directory).listFiles();
            if (files != null) for (File file : files) file.delete();
        }
        new File(dataDir, REQUEST).delete();
        new File(dataDir, SHOWING).delete();
    }

    /** A retired request must not be opened after its caller's result was answered. */
    static void forgetResult(String token) {
        if (!validToken(token)) return;
        File[] files = new File(Device.dataDir(), REQUESTS)
                .listFiles((directory, name) -> name.endsWith("-" + token + ".request"));
        if (files != null) for (File file : files) file.delete();
        new File(new File(Device.dataDir(), RESULTS), token).delete();
    }

    /** The request: one line, the package (empty for the main view); replaced atomically. */
    static synchronized void writeRequest(File request, String packageName) throws IOException {
        writeAtomically(request, packageName);
    }

    /** One line, replaced atomically. */
    private static void writeAtomically(File target, String line) throws IOException {
        File dir = target.getParentFile();
        if (dir != null && !dir.isDirectory() && !dir.mkdirs() && !dir.isDirectory()) {
            throw new IOException("cannot create " + dir);
        }
        File temp = File.createTempFile(target.getName() + ".", ".tmp", dir);
        try {
            try (FileOutputStream out = new FileOutputStream(temp)) {
                out.write((line == null ? "" : line).getBytes(StandardCharsets.UTF_8));
                out.write('\n');
            }
            try {
                Files.move(temp.toPath(), target.toPath(), StandardCopyOption.ATOMIC_MOVE,
                        StandardCopyOption.REPLACE_EXISTING);
            } catch (AtomicMoveNotSupportedException e) {
                Files.move(temp.toPath(), target.toPath(), StandardCopyOption.REPLACE_EXISTING);
            }
        } finally {
            temp.delete();
        }
    }
}
