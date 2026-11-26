/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
import java.lang.instrument.Instrumentation;

/** Read actual shallow sizes only; no transformation or timed-query instrumentation. */
public final class PackageMetadataSizeAgent {
    private static Instrumentation instrumentation;
    public static void premain(String ignored, Instrumentation value) { instrumentation = value; }
    public static long size(Object value) {
        if (instrumentation == null) throw new AssertionError("Size agent was not installed");
        return value == null ? 0 : instrumentation.getObjectSize(value);
    }
}
