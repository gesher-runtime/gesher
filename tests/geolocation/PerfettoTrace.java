/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package android.os;
/** Host diagnostic boundary: no native trace collection. */
public final class PerfettoTrace {
    public static final boolean IS_USE_SDK_TRACING_API_V3 = false;
    public static final Category MQ_CATEGORY = new Category();
    public static final class Category { }
    public static boolean isMQCategoryEnabled() { return false; }
    public static PerfettoTrackEventExtra.Builder counter(Category category, long count) {
        return new PerfettoTrackEventExtra.Builder();
    }
}
