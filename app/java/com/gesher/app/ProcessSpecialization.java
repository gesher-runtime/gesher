/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app;

import android.content.pm.ApplicationInfo;
import android.system.ErrnoException;
import android.system.Os;

/**
 * The part of the zygote's specialisation of an app process that ptrace sees
 * (com_android_internal_os_Zygote.cpp, SpecializeCommon; ADR 0093): an app process is not
 * dumpable (its uid change cleared the flag, and the zygote clears what is left for an app
 * uid), so no other process of its uid may attach to it, unless the app is debuggable
 * ({@code DEBUG_ENABLE_PTRACE}: dumpable, and any process may attach, as for a native
 * debugger) or profileable from the shell ({@code PROFILE_FROM_SHELL}: dumpable). The
 * app's own native code may still change the flag with {@code prctl}.
 */
public final class ProcessSpecialization {
    /** Zygote's runtime flags (com.android.internal.os.Zygote) this class acts on. */
    public static final int PROFILE_FROM_SHELL = 1 << 15;
    public static final int DEBUG_ENABLE_PTRACE = 1 << 25;

    private static final int PR_SET_DUMPABLE = 4;
    private static final int PR_SET_PTRACER = 0x59616d61;
    private static final long PR_SET_PTRACER_ANY = -1L;

    private ProcessSpecialization() {
    }

    /** The runtime flags ActivityManagerService's ProcessList gives a process of the app. */
    public static int runtimeFlagsFor(ApplicationInfo info) {
        int flags = 0;
        if ((info.flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0) flags |= DEBUG_ENABLE_PTRACE;
        if (info.isProfileableByShell()) flags |= PROFILE_FROM_SHELL;
        return flags;
    }

    /** Sets this app process's dumpable flag (and ptracer exception) for {@code flags}. */
    public static void apply(int flags, String tag) {
        try {
            Os.prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
            if ((flags & DEBUG_ENABLE_PTRACE) != 0) {
                // The zygote's EnableDebugger.
                Os.prctl(PR_SET_DUMPABLE, 1, 0, 0, 0);
                Os.prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
            }
            if ((flags & PROFILE_FROM_SHELL) != 0) Os.prctl(PR_SET_DUMPABLE, 1, 0, 0, 0);
        } catch (ErrnoException e) {
            android.util.Log.w(tag, "process specialisation: prctl failed", e);
        }
    }
}
