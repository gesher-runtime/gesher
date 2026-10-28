/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import java.io.File;
import java.util.List;

/**
 * The Gesher device's processes: a zygote, lite. Windows has no fork, so an app process
 * is a new dalvikvm, started with this process's own runtime options (its command line up
 * to the main class) and {@link android.app.GesherAppProcess} as the main class, given its
 * package and a start sequence number it attaches with, as ActivityManagerService's
 * ProcessList asks the zygote for a process and pairs it by {@code startSeq}.
 *
 * <p>The natives (libandroid_runtime: compat/framework/services_zygote.cc) start a process
 * detached (no console window, a process group of its own, stdout and stderr appended to a
 * log file, no inherited handle but those), tell whether a process runs, end one, and keep
 * a job that ends everything in it with this process (the system server's: binderd,
 * servicemanager and the app processes go when it goes).
 */
public final class Zygote {
    private Zygote() {
    }

    static native int nativeSpawn(String commandLine, String[] setEnv, String[] unsetEnv,
            String logPath, boolean breakaway, int uid, String packageName, String installInstance);
    static native boolean nativeRevokeInstall(String packageName, String installInstance);
    private static native int[] nativeStartDevice(String[] commands, String[] logs, String[] env);
    static native boolean nativeIsAuthority();
    static native boolean nativeOwnedInstall(int pid, String packageName, String installInstance);

    /** Creates one owned private bootstrap; no capability appears in these strings. */
    public static int[] startDevice(List<String> commands, List<String> logs, List<String> env) {
        String[] paths = logs.stream().map(Zygote::windowsPath).toArray(String[]::new);
        int[] children = nativeStartDevice(commands.toArray(new String[0]), paths,
                env.toArray(new String[0]));
        if (children == null || children.length != 3) {
            throw new IllegalStateException("private device bootstrap failed");
        }
        // Honest original host only. Broker UID comes from its prepublished owned record.
        try { android.system.Os.setenv("GESHER_BINDER_UID", "2000", true); }
        catch (Exception e) { throw new IllegalStateException(e); }
        return children;
    }
    static native long nativeStartTime(int pid);
    static native int nativeExitCode(int pid);
    static native long nativeExitStatus(int pid);
    static native boolean nativeKill(int pid);
    static native boolean nativeJoinJob(int pid);
    static native long nativeLock(String name, int timeoutMs);
    static native void nativeUnlock(long handle);
    static native long nativeCreateEvent(String name);
    static native boolean nativeWaitEvent(long handle, int timeoutMs);
    static native boolean nativeSignalEvent(String name);
    static native boolean nativeWaitForBinder(String device, int pid, int timeoutMs);
    static native String nativeCommandLine();
    static native String nativeExePath();

    public static final long EXIT_RUNNING = -2;
    public static final long EXIT_UNKNOWN = -1;

    /** Legacy ambiguous sentinel; lifetime observers use exitStatus. */
    public static final int STILL_ACTIVE = 259;

    /**
     * Starts {@code commandLine} detached, its output appended to {@code logPath}, with
     * {@code GESHER_BINDER_UID} (the system server's) left out of its environment and
     * {@code setEnv} ({@code NAME=value}) added. Returns the pid.
     */
    public static int spawn(String commandLine, List<String> setEnv, String logPath,
            boolean breakaway) {
        return spawnWithIdentity(commandLine, setEnv, logPath, breakaway, null);
    }

    /** Explicit per-child self/Binder compatibility snapshot; never broker authority. */
    static int spawnPackage(String commandLine, List<String> setEnv, String logPath,
            PackageIdentity identity) {
        return spawnWithIdentity(commandLine, setEnv, logPath, false, identity);
    }

    private static int spawnWithIdentity(String commandLine, List<String> setEnv, String logPath,
            boolean breakaway, PackageIdentity identity) {
        new File(logPath).getParentFile().mkdirs();
        int pid = nativeSpawn(commandLine, setEnv.toArray(new String[0]),
                identity == null ? new String[] {"GESHER_BINDER_UID"}
                        : new String[] {"GESHER_BINDER_UID", "GESHER_UID"}, windowsPath(logPath),
                breakaway, identity == null ? -1 : identity.uid,
                identity == null ? "" : identity.packageName,
                identity == null ? "" : identity.installInstance);
        if (pid <= 0) {
            throw new IllegalStateException("starting " + commandLine + " failed: Win32 error "
                    + (-pid));
        }
        return pid;
    }

    /** The process's creation time, 0 when it does not run. */
    public static long startTime(int pid) {
        return pid <= 0 ? 0 : nativeStartTime(pid);
    }

    /** Whether {@code pid} runs and is the process created at {@code startTime}. */
    public static boolean alive(int pid, long startTime) {
        long t = startTime(pid);
        return t != 0 && (startTime == 0 || t == startTime);
    }

    /** Full unsigned terminal code, EXIT_RUNNING, or EXIT_UNKNOWN on lookup/API failure.
     * Completion consumes the owned child; terminal 259 is distinct from running. */
    public static long exitStatus(int pid) { return nativeExitStatus(pid); }

    /** Legacy ambiguous API retained for binary compatibility; prefer exitStatus. */
    public static int exitCode(int pid) {
        return nativeExitCode(pid);
    }

    public static boolean kill(int pid) {
        return pid > 0 && nativeKill(pid);
    }

    /** Puts {@code pid} (0: this process) in this process's kill-on-close job. */
    public static boolean joinJob(int pid) {
        return nativeJoinJob(pid);
    }

    public static long lock(String name, int timeoutMs) {
        return nativeLock(name, timeoutMs);
    }

    public static void unlock(long handle) {
        nativeUnlock(handle);
    }

    public static long createEvent(String name) {
        return nativeCreateEvent(name);
    }

    public static boolean waitEvent(long handle, int timeoutMs) {
        return nativeWaitEvent(handle, timeoutMs);
    }

    public static boolean signalEvent(String name) {
        return nativeSignalEvent(name);
    }
    public static boolean waitForBinder(String device, int pid, int timeoutMs) {
        return nativeWaitForBinder(device, pid, timeoutMs);
    }

    /** The full path of this process's executable (dalvikvm.exe). */
    public static String exePath() {
        return nativeExePath();
    }

    /**
     * This dalvikvm's command line up to {@code mainClass}: the executable and the runtime's
     * options (boot class path, image, interpreter), to start another dalvikvm the same way.
     */
    public static String vmCommandLine(String mainClass) {
        return com.gesher.app.VmCommandLine.beforeMainClass(nativeCommandLine(), mainClass);
    }

    /** The command line of a dalvikvm like this one running {@code mainClass args}. */
    public static String commandLine(String vmCommandLine, String mainClass, List<String> args) {
        StringBuilder sb = new StringBuilder(vmCommandLine).append(' ').append(mainClass);
        for (String a : args) sb.append(' ').append(quote(a));
        return sb.toString();
    }

    /** An argument quoted as CommandLineToArgvW reads it. */
    public static String quote(String arg) {
        if (!arg.isEmpty() && arg.indexOf(' ') < 0 && arg.indexOf('\t') < 0
                && arg.indexOf('"') < 0) {
            return arg;
        }
        StringBuilder sb = new StringBuilder("\"");
        int backslashes = 0;
        for (int i = 0; i < arg.length(); i++) {
            char c = arg.charAt(i);
            if (c == '\\') {
                backslashes++;
                continue;
            }
            if (c == '"') backslashes = backslashes * 2 + 1;
            for (; backslashes > 0; backslashes--) sb.append('\\');
            sb.append(c);
        }
        for (backslashes *= 2; backslashes > 0; backslashes--) sb.append('\\');
        return sb.append('"').toString();
    }

    /**
     * A path for Win32: a drive-less path (ADR 0010: paths on the current drive) gets the
     * current drive, '/' becomes '\'.
     */
    public static String windowsPath(String path) {
        String p = new File(path).getAbsolutePath().replace('/', '\\');
        if (p.startsWith("\\") && !p.startsWith("\\\\")) {
            String exe = exePath();
            if (exe != null && exe.length() > 2 && exe.charAt(1) == ':') {
                p = exe.substring(0, 2) + p;
            }
        }
        return p;
    }
}
