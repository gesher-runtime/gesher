/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.net;

/**
 * A placeholder for the Connectivity module's Network, for {@link ConnectivityManager}'s
 * signatures: with no network, none is ever made.
 */
public class Network {
    private final int mNetId;

    /** @hide */
    public Network(int netId) {
        mNetId = netId;
    }

    /** @hide */
    public int getNetId() {
        return mNetId;
    }

    public long getNetworkHandle() {
        return mNetId == 0 ? 0 : ((long) mNetId << 32) | 0xfacade;
    }

    @Override
    public boolean equals(Object o) {
        return o instanceof Network && ((Network) o).mNetId == mNetId;
    }

    @Override
    public int hashCode() {
        return mNetId * 11;
    }

    @Override
    public String toString() {
        return Integer.toString(mNetId);
    }
}
