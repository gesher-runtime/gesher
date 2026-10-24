/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.util;

/**
 * Placeholder for the statsd module's StatsEvent, which framework.jar is compiled against
 * but does not contain (app/README.md, "Classes framework.jar does not have"): the
 * framework's generated *StatsLog classes build events with it (ViewRootImpl logs its color
 * mode through HwuiStatsLog). There is no statsd: an event keeps its atom id only, and
 * {@link StatsLog#write} drops it. When the module's class is on the boot class path ahead
 * of gesher-app.jar, it wins.
 */
public final class StatsEvent {
    private final int mAtomId;

    private StatsEvent(int atomId) {
        mAtomId = atomId;
    }

    public static Builder newBuilder() {
        return new Builder();
    }

    public int getAtomId() {
        return mAtomId;
    }

    public byte[] getBytes() {
        return new byte[0];
    }

    public int getNumBytes() {
        return 0;
    }

    public void release() {
    }

    /** Builds an event; the fields are not kept. */
    public static final class Builder {
        private int mAtomId;

        Builder() {
        }

        public Builder setAtomId(int atomId) {
            mAtomId = atomId;
            return this;
        }

        public Builder usePooledBuffer() { return this; }
        public Builder addBooleanAnnotation(byte annotationId, boolean value) { return this; }
        public Builder addIntAnnotation(byte annotationId, int value) { return this; }
        public Builder writeAttributionChain(int[] uids, String[] tags) { return this; }
        public Builder writeBoolean(boolean value) { return this; }
        public Builder writeBooleanArray(boolean[] values) { return this; }
        public Builder writeByteArray(byte[] value) { return this; }
        public Builder writeFloat(float value) { return this; }
        public Builder writeFloatArray(float[] values) { return this; }
        public Builder writeInt(int value) { return this; }
        public Builder writeIntArray(int[] values) { return this; }
        public Builder writeLong(long value) { return this; }
        public Builder writeLongArray(long[] values) { return this; }
        public Builder writeString(String value) { return this; }
        public Builder writeStringArray(String[] values) { return this; }

        public Builder writeKeyValuePairs(SparseIntArray intMap, SparseLongArray longMap,
                SparseArray<String> stringMap, SparseArray<Float> floatMap) {
            return this;
        }

        public StatsEvent build() {
            return new StatsEvent(mAtomId);
        }
    }
}
