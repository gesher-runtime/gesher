/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app.shell;

/**
 * The Windows shell, for {@link AppShell}: Start menu shortcuts (IShellLinkW, with
 * System.AppUserModel.ID in their property store), this process's AppUserModelID, the
 * file operations on the Start menu folder, and the apps' URL protocols (per user, in
 * HKCU\Software\Classes). Paths are Windows paths with a drive letter: the
 * Start menu may be on another drive than Gesher, which java.io's drive-less paths (ADR 0010)
 * cannot reach. The natives are in compat/shell (libandroid_runtime's builtin library).
 */
public final class Shell {
    private Shell() {
    }

    /** The user's Start menu Programs folder (FOLDERID_Programs), or null. */
    public static String programsFolder() {
        return nativeProgramsFolder();
    }

    /**
     * Creates or replaces the shortcut {@code link}. Returns 0 (S_OK) or the HRESULT that
     * failed.
     */
    public static int createShortcut(String link, String target, String arguments,
            String workDir, String icon, String description, String appUserModelId) {
        return nativeCreateShortcut(link, target, arguments, workDir, icon, description,
                appUserModelId);
    }

    /** SetCurrentProcessExplicitAppUserModelID; returns its HRESULT. */
    public static int setProcessAppUserModelId(String id) {
        return nativeSetProcessAppUserModelId(id);
    }

    /** GetCurrentProcessExplicitAppUserModelID, or null. */
    public static String processAppUserModelId() {
        return nativeProcessAppUserModelId();
    }

    public static boolean makeDirectories(String dir) {
        return nativeMakeDirectories(dir);
    }

    /** Deletes a file; true when it is gone. */
    public static boolean removeFile(String path) {
        return nativeRemoveFile(path);
    }

    /** Removes a directory if it is empty; true when it is gone. */
    public static boolean removeEmptyDirectory(String dir) {
        return nativeRemoveEmptyDirectory(dir);
    }

    public static boolean exists(String path) {
        return nativeExists(path);
    }

    /** Tells the shell the file changed: Explorer reads its icon again. */
    public static void notifyChanged(String path) {
        nativeNotifyChanged(path);
    }

    /** {@link #registerUrlProtocol}'s HRESULT for a scheme that is not the package's. */
    public static final int ALREADY_EXISTS = 0x800700B7;  // HRESULT_FROM_WIN32(183)

    /**
     * Registers the URL protocol {@code scheme} for the package {@code owner}: Windows runs
     * {@code command} ({@code %1}: the URL) for its URLs. A scheme that is another's (not
     * Gesher's, or another package's) is left alone: {@link #ALREADY_EXISTS}. Returns 0
     * (S_OK) or the HRESULT that failed.
     */
    public static int registerUrlProtocol(String scheme, String owner, String description,
            String command, String icon) {
        return nativeRegisterUrlProtocol(scheme, owner, description, command, icon);
    }

    /**
     * Removes the URL protocol {@code scheme} if it is {@code owner}'s. Returns 0 (S_OK) when
     * it was removed, 1 (S_FALSE) when it is not the package's, or the HRESULT that failed.
     */
    public static int unregisterUrlProtocol(String scheme, String owner) {
        return nativeUnregisterUrlProtocol(scheme, owner);
    }

    /** The package whose URL protocol {@code scheme} is, or null. */
    public static String urlProtocolOwner(String scheme) {
        return nativeUrlProtocolOwner(scheme);
    }

    private static native String nativeProgramsFolder();
    private static native int nativeCreateShortcut(String link, String target, String arguments,
            String workDir, String icon, String description, String appUserModelId);
    private static native int nativeSetProcessAppUserModelId(String id);
    private static native String nativeProcessAppUserModelId();
    private static native boolean nativeMakeDirectories(String dir);
    private static native boolean nativeRemoveFile(String path);
    private static native boolean nativeRemoveEmptyDirectory(String dir);
    private static native boolean nativeExists(String path);
    private static native void nativeNotifyChanged(String path);
    private static native int nativeRegisterUrlProtocol(String scheme, String owner,
            String description, String command, String icon);
    private static native int nativeUnregisterUrlProtocol(String scheme, String owner);
    private static native String nativeUrlProtocolOwner(String scheme);
}
