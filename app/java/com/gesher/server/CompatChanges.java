/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import com.gesher.server.gen.CompatChangeTable;

import java.util.Arrays;

/**
 * The framework's app-compat changes for a target SDK, as PlatformCompat answers them for
 * bindApplication's {@code disabledCompatChanges} (docs/e2e.md): a change gated on a target
 * SDK is enabled only for apps that target it, so an app keeps the behavior it was built
 * for. Without this every change is enabled for every app: e.g. DISABLE_OPT_OUT_EDGE_TO_EDGE
 * (enabled since SDK 36) made apps targeting 35 lose windowOptOutEdgeToEdgeEnforcement and
 * draw their content under the action bar.
 *
 * <p>The table is generated at build time from framework.jar's {@code @ChangeId} fields
 * (app/gen/CompatChangesGen.java); overrides (adb's {@code am compat}) do not exist.
 */
public final class CompatChanges {
    /** SharedLibrariesImpl's @ChangeId / @EnabledAfter(R), Android 16 r4. */
    public static final long ENFORCE_NATIVE_SHARED_LIBRARY_DEPENDENCIES = 142191088L;

    // Service-only annotations can be absent from a framework.jar/source
    // snapshot. Keep this PM rule in the same policy used by bindApplication,
    // rather than independently guessing the target SDK in the installer.
    private static final long[][] SERVICE_CHANGES = {
            {ENFORCE_NATIVE_SHARED_LIBRARY_DEPENDENCIES, 3, 30}
    };

    private CompatChanges() {
    }

    /** The changes disabled for an app targeting {@code targetSdk}, sorted. */
    public static long[] disabledFor(int targetSdk) {
        long[] out = new long[CompatChangeTable.CHANGES.length + SERVICE_CHANGES.length];
        int n = 0;
        for (long[] c : CompatChangeTable.CHANGES) {
            if (!enabled(c, targetSdk)) out[n++] = c[0];
        }
        for (long[] c : SERVICE_CHANGES) {
            if (generated(c[0]) == null && !enabled(c, targetSdk)) out[n++] = c[0];
        }
        long[] disabled = Arrays.copyOf(out, n);
        Arrays.sort(disabled);
        return disabled;
    }

    /** The current generated compat policy; unknown changes follow Android's enabled default. */
    public static boolean isEnabled(long changeId, int targetSdk) {
        long[] change = generated(changeId);
        if (change != null) return enabled(change, targetSdk);
        for (long[] service : SERVICE_CHANGES) {
            if (service[0] == changeId) return enabled(service, targetSdk);
        }
        return true;
    }

    private static long[] generated(long id) {
        for (long[] change : CompatChangeTable.CHANGES) if (change[0] == id) return change;
        return null;
    }

    private static boolean enabled(long[] change, int targetSdk) {
        int kind = (int) change[1], sdk = (int) change[2];
        return !(kind == 1 || kind == 2 && targetSdk < sdk || kind == 3 && targetSdk <= sdk);
    }

    /** How many changes the table has. */
    public static int count() {
        int count = CompatChangeTable.CHANGES.length;
        for (long[] service : SERVICE_CHANGES) if (generated(service[0]) == null) ++count;
        return count;
    }
}
