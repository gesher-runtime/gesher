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
 * the caller; for a startActivityForResult, a {@link #RESULTS} file holds the
 * {@link SettingsResults} token the Control Center reports when the app's details close);
 * the Control Center reads and deletes it as it starts and on its live-refresh
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
    /**
     * Requests awaiting their result (SettingsResults): one file per token, named by it and
     * holding the package. The request file itself stays one package name, so a Control
     * Center of an older Gesher still shows the details (it reports nothing: the result then
     * comes at the timeout); a current one takes the files of the package it shows.
     */
    static final String RESULTS = "system/control-panel-results";
    /**
     * The package whose details the Control Center shows for a request, written by it before
     * it takes the request and removed when the details close (gesher-control-panel.ps1).
     */
    static final String SHOWING = "system/control-panel-showing";
    private static final String SHOWING_NAME = "control-panel-showing";

    /** Opens the Control Center for a request awaiting a result. */
    interface Opener {
        /**
         * @param replaced told the tokens of requests for other packages that this one
         *     replaced before the Control Center showed them: they will not be shown
         */
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
        File results = new File(Device.dataDir(), RESULTS);
        try {
            publishRequest(request, results, packageName, token, replaced);
        } catch (IOException e) {
            SystemServer.log("Control Center: no request file: " + e);
            if (token != null) new File(results, token).delete();
            return false;
        }
        boolean opened = false;
        try {
            opened = WindowsHost.openUrl(link);
            return opened;
        } finally {
            // Serialize publication and failure cleanup: never delete a later request.
            if (!opened) {
                request.delete();
                if (token != null) new File(results, token).delete();
            }
        }
    }

    /**
     * Publishes a request: its token file first (the Control Center may take the request at
     * once), then the request file. The token files of other packages are requests the new
     * one replaces before they were shown: they are removed and told to {@code replaced}.
     * A request for the same package keeps them (its details take them all).
     */
    static synchronized void publishRequest(File request, File results, String packageName,
            String token, java.util.function.Consumer<String> replaced) throws IOException {
        if (token != null && (packageName == null || !validToken(token))) {
            throw new IOException("invalid token");
        }
        boolean published = false;
        try {
            if (token != null) writeAtomically(new File(results, token), packageName);
            // The package whose details the Control Center shows (or is taking): its tokens
            // are being answered by those details, never replaced.
            String showing = null;
            try {
                showing = new String(Files.readAllBytes(new File(request.getParentFile(),
                        SHOWING_NAME).toPath()), StandardCharsets.UTF_8).trim();
            } catch (IOException none) {
                // no details shown
            }
            File[] waiting = results.listFiles();
            if (waiting != null) {
                for (File file : waiting) {
                    String name = file.getName();
                    if (!validToken(name) || name.equals(token)) continue;
                    String owner;
                    try {
                        owner = new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8).trim();
                    } catch (IOException e) {
                        continue;  // taken by the Control Center meanwhile
                    }
                    if (owner.equals(packageName) || owner.equals(showing)) continue;
                    if (file.delete() && replaced != null) replaced.accept(name);
                }
            }
            writeRequest(request, packageName);
            published = true;
        } finally {
            // Whatever failed, no token file outlives a request that was not published.
            if (!published && token != null) new File(results, token).delete();
        }
    }

    /**
     * A new device: no request from an earlier one waits (their callers are gone), and no
     * details are shown yet. Removes the token files, leftover temporary files and the
     * showing marker.
     */
    static void clearResults(String dataDir) {
        File[] files = new File(dataDir, RESULTS).listFiles();
        if (files != null) for (File file : files) file.delete();
        new File(dataDir, SHOWING).delete();
    }

    /** The request's result was answered (or timed out): its token file goes. */
    static void forgetResult(String token) {
        if (validToken(token)) new File(new File(Device.dataDir(), RESULTS), token).delete();
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
