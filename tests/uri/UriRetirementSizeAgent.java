/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import java.lang.instrument.Instrumentation;

/** Used only by the separate retained-layout observation, never ordinary timing. */
public final class UriRetirementSizeAgent {
    private static Instrumentation instrumentation;
    public static void premain(String unused, Instrumentation value) { instrumentation = value; }
    public static long size(Object value) {
        if (instrumentation == null) throw new IllegalStateException("Run the separate size-agent entry");
        return instrumentation.getObjectSize(value);
    }
}
