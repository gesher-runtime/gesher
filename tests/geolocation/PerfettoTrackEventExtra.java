/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package android.os;
/** Host diagnostic boundary for MessageQueue's unconditional count trace. */
public final class PerfettoTrackEventExtra {
    public static final class Builder {
        public Builder usingThreadCounterTrack(long tid, String name) { return this; }
        public void emit() { }
    }
}
