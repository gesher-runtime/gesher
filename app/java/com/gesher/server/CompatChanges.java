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
    private CompatChanges() {
    }

    /** The changes disabled for an app targeting {@code targetSdk}, sorted. */
    public static long[] disabledFor(int targetSdk) {
        long[] out = new long[CompatChangeTable.CHANGES.length];
        int n = 0;
        for (long[] c : CompatChangeTable.CHANGES) {
            int kind = (int) c[1];
            int sdk = (int) c[2];
            boolean disabled = kind == 1  // @Disabled
                    || (kind == 2 && targetSdk < sdk)  // @EnabledSince
                    || (kind == 3 && targetSdk <= sdk);  // @EnabledAfter
            if (disabled) out[n++] = c[0];
        }
        long[] disabled = Arrays.copyOf(out, n);
        Arrays.sort(disabled);
        return disabled;
    }

    /** How many changes the table has. */
    public static int count() {
        return CompatChangeTable.CHANGES.length;
    }
}
