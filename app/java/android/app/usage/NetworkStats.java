/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package android.app.usage;

/** Result types for unavailable network accounting; no query constructs a result. */
public final class NetworkStats implements AutoCloseable {
    private NetworkStats() {}

    public boolean hasNextBucket() { throw unavailable(); }
    public boolean getNextBucket(Bucket bucket) { throw unavailable(); }
    @Override public void close() { }

    /** An unpopulated output container is not a zero-usage measurement. */
    public static class Bucket {
        public Bucket() {}
        public int getUid() { throw unavailable(); }
        public int getState() { throw unavailable(); }
        public int getRoaming() { throw unavailable(); }
        public int getMetered() { throw unavailable(); }
        public long getStartTimeStamp() { throw unavailable(); }
        public long getRxBytes() { throw unavailable(); }
        public long getTxBytes() { throw unavailable(); }
    }

    private static UnsupportedOperationException unavailable() {
        return new UnsupportedOperationException("No Android network usage measurements are available");
    }
}
