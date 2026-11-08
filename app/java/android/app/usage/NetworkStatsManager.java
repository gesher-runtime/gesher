/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package android.app.usage;

import android.content.Context;
import android.os.RemoteException;
import java.util.Objects;

/**
 * Unavailable Android UID-based network accounting. The optional Connectivity
 * module wrapper exists so clients can handle its documented RemoteException;
 * it supplies no host traffic, subscriber information or fabricated measurements.
 */
public final class NetworkStatsManager {
    public NetworkStatsManager(Context context) {
        Objects.requireNonNull(context);
    }

    public NetworkStats querySummary(int networkType, String subscriberId, long startTime, long endTime)
            throws RemoteException {
        throw unavailable();
    }

    public NetworkStats.Bucket querySummaryForDevice(int networkType, String subscriberId,
            long startTime, long endTime) throws RemoteException {
        throw unavailable();
    }

    private static RemoteException unavailable() {
        return new RemoteException("Android network usage accounting is unavailable on Gesher");
    }
}
