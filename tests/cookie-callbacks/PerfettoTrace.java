/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package android.os;
/** Host-only diagnostic boundary: no trace collection on the host JVM. */
public final class PerfettoTrace {
    public static boolean isMQCategoryEnabled() { return false; }
}
