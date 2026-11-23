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

/**
 * Opens Gesher's Control Center (tools/package/gesher-control-panel.ps1), optionally on one
 * app's details: Android's "App info", which Gesher has no Settings APK for
 * ({@link SettingsIntents}).
 *
 * <p>The app goes in {@code system/control-panel-request} (one package name, validated by
 * the caller, and for a startActivityForResult a tab and the {@link SettingsResults} token
 * the Control Center reports when the app's details close); the Control Center reads and deletes it as it starts and on its live-refresh
 * timer, so a Control Center already open shows the app too. ShellExecute takes no
 * arguments here ({@link WindowsHost#openUrl}), so the Control Center is opened through a
 * shortcut to {@code gesher-launch.exe control-panel} that this class keeps next to the
 * request; a second Control Center leaves the request to the first and exits.
 */
final class ControlCenter {
    private ControlCenter() {
    }

    static final String REQUEST = "system/control-panel-request";
    static final String SHORTCUT = "system/gesher-control-panel.lnk";

    /** Replaced by tests: opens the Control Center on a package (null: its main view). */
    static volatile java.util.function.Predicate<String> sOpen = pkg -> openOnHost(pkg, null);
    /** Replaced by tests: opens it on a package for a request awaiting its result token. */
    static volatile java.util.function.BiPredicate<String, String> sOpenForResult =
            ControlCenter::openOnHost;

    static boolean open(String packageName) {
        return open(packageName, null);
    }

    /** With a token (SettingsResults), the Control Center reports when the details close. */
    static boolean open(String packageName, String token) {
        try {
            if (token == null) return sOpen.test(packageName);
            // SettingsResults.validToken, kept here: the host request test builds this class alone.
            if (packageName == null || !token.matches("[0-9a-f]{32}")) return false;
            return sOpenForResult.test(packageName, token);
        } catch (RuntimeException | LinkageError e) {
            SystemServer.log("Control Center: " + e);
            return false;
        }
    }

    private static synchronized boolean openOnHost(String packageName, String token) {
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
        try {
            writeRequest(request, packageName, token);
        } catch (IOException e) {
            SystemServer.log("Control Center: no request file: " + e);
            return false;
        }
        boolean opened = false;
        try {
            opened = WindowsHost.openUrl(link);
            return opened;
        } finally {
            // Serialize publication and failure cleanup: never delete a later request.
            if (!opened) request.delete();
        }
    }

    /** The request: one line, the package (empty for the main view); replaced atomically. */
    static synchronized void writeRequest(File request, String packageName) throws IOException {
        writeRequest(request, packageName, null);
    }

    /** One line: the package, then a tab and the result token when there is one. */
    static synchronized void writeRequest(File request, String packageName, String token)
            throws IOException {
        File dir = request.getParentFile();
        if (dir != null && !dir.isDirectory() && !dir.mkdirs() && !dir.isDirectory()) {
            throw new IOException("cannot create " + dir);
        }
        File temp = File.createTempFile(request.getName() + ".", ".tmp", dir);
        try {
            try (FileOutputStream out = new FileOutputStream(temp)) {
                out.write((packageName == null ? "" : packageName).getBytes(StandardCharsets.UTF_8));
                if (token != null && packageName != null) {
                    out.write('\t');
                    out.write(token.getBytes(StandardCharsets.UTF_8));
                }
                out.write('\n');
            }
            try {
                Files.move(temp.toPath(), request.toPath(), StandardCopyOption.ATOMIC_MOVE,
                        StandardCopyOption.REPLACE_EXISTING);
            } catch (AtomicMoveNotSupportedException e) {
                Files.move(temp.toPath(), request.toPath(), StandardCopyOption.REPLACE_EXISTING);
            }
        } finally {
            temp.delete();
        }
    }
}
