/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.net;

/**
 * A placeholder for the Connectivity module's NetworkCapabilities, for
 * {@link ConnectivityManager}'s and {@link NetworkRequest}'s signatures: capabilities and
 * transports are recorded, and no network has any.
 */
public final class NetworkCapabilities {
    public static final int NET_CAPABILITY_MMS = 0;
    public static final int NET_CAPABILITY_SUPL = 1;
    public static final int NET_CAPABILITY_DUN = 2;
    public static final int NET_CAPABILITY_FOTA = 3;
    public static final int NET_CAPABILITY_IMS = 4;
    public static final int NET_CAPABILITY_CBS = 5;
    public static final int NET_CAPABILITY_WIFI_P2P = 6;
    public static final int NET_CAPABILITY_IA = 7;
    public static final int NET_CAPABILITY_RCS = 8;
    public static final int NET_CAPABILITY_XCAP = 9;
    public static final int NET_CAPABILITY_EIMS = 10;
    public static final int NET_CAPABILITY_NOT_METERED = 11;
    public static final int NET_CAPABILITY_INTERNET = 12;
    public static final int NET_CAPABILITY_NOT_RESTRICTED = 13;
    public static final int NET_CAPABILITY_TRUSTED = 14;
    public static final int NET_CAPABILITY_NOT_VPN = 15;
    public static final int NET_CAPABILITY_VALIDATED = 16;
    public static final int NET_CAPABILITY_CAPTIVE_PORTAL = 17;
    public static final int NET_CAPABILITY_NOT_ROAMING = 18;
    public static final int NET_CAPABILITY_FOREGROUND = 19;
    public static final int NET_CAPABILITY_NOT_CONGESTED = 20;
    public static final int NET_CAPABILITY_NOT_SUSPENDED = 21;
    public static final int NET_CAPABILITY_TEMPORARILY_NOT_METERED = 25;
    public static final int TRANSPORT_CELLULAR = 0;
    public static final int TRANSPORT_WIFI = 1;
    public static final int TRANSPORT_BLUETOOTH = 2;
    public static final int TRANSPORT_ETHERNET = 3;
    public static final int TRANSPORT_VPN = 4;
    public static final int TRANSPORT_WIFI_AWARE = 5;
    public static final int TRANSPORT_LOWPAN = 6;
    public static final int TRANSPORT_USB = 8;
    public static final int TRANSPORT_THREAD = 9;
    public static final int TRANSPORT_SATELLITE = 10;

    private long mCapabilities;
    private long mTransports;

    public NetworkCapabilities() {
    }

    public NetworkCapabilities(NetworkCapabilities other) {
        if (other != null) {
            mCapabilities = other.mCapabilities;
            mTransports = other.mTransports;
        }
    }

    /** @hide */
    public NetworkCapabilities addCapability(int capability) {
        mCapabilities |= 1L << capability;
        return this;
    }

    /** @hide */
    public NetworkCapabilities removeCapability(int capability) {
        mCapabilities &= ~(1L << capability);
        return this;
    }

    /** @hide */
    public NetworkCapabilities addTransportType(int transportType) {
        mTransports |= 1L << transportType;
        return this;
    }

    /** @hide */
    public NetworkCapabilities removeTransportType(int transportType) {
        mTransports &= ~(1L << transportType);
        return this;
    }

    public boolean hasCapability(int capability) {
        return capability >= 0 && capability < 64 && (mCapabilities & (1L << capability)) != 0;
    }

    public boolean hasTransport(int transportType) {
        return transportType >= 0 && transportType < 64
                && (mTransports & (1L << transportType)) != 0;
    }

    public int[] getCapabilities() {
        return bits(mCapabilities);
    }

    public int[] getTransportTypes() {
        return bits(mTransports);
    }

    public int getLinkDownstreamBandwidthKbps() {
        return 0;
    }

    public int getLinkUpstreamBandwidthKbps() {
        return 0;
    }

    public int getSignalStrength() {
        return Integer.MIN_VALUE;
    }

    private static int[] bits(long mask) {
        int[] out = new int[Long.bitCount(mask)];
        for (int i = 0, n = 0; i < 64; i++) {
            if ((mask & (1L << i)) != 0) out[n++] = i;
        }
        return out;
    }
}
