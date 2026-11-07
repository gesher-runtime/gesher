/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package android.app.usage;

/** Result types for unavailable network accounting; no query constructs a result. */
public final class NetworkStats implements AutoCloseable {
    private NetworkStats() {}

    public boolean hasNextBucket() { return false; }
    public boolean getNextBucket(Bucket bucket) { return false; }
    @Override public void close() { }

    /** An unpopulated output container is not a zero-usage measurement. */
    public static class Bucket {
        public Bucket() {}
        public int getUid() { return -1; }
        public int getState() { return 0; }
        public int getRoaming() { return 0; }
        public int getMetered() { return 0; }
        public long getStartTimeStamp() { return 0L; }
        public long getRxBytes() { return 0L; }
        public long getTxBytes() { return 0L; }
    }
}
