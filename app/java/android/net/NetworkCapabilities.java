/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.net;

import android.os.Parcel;
import android.os.Parcelable;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Objects;

/**
 * The Connectivity module's NetworkCapabilities (framework.jar compiles against the
 * module's API and does not contain it), with its public API: what a network can do
 * (capabilities), what it runs over (transports), and its bandwidth (docs/network.md).
 * {@link ConnectivityManager} fills it from Windows: Wi-Fi, Ethernet, cellular or VPN;
 * INTERNET when Windows routes beyond the network, VALIDATED when the Network List
 * Manager reaches the internet through it, NOT_METERED and NOT_ROAMING from its cost.
 */
public final class NetworkCapabilities implements Parcelable {
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
    public static final int NET_CAPABILITY_OEM_PAID = 22;
    public static final int NET_CAPABILITY_MCX = 23;
    public static final int NET_CAPABILITY_PARTIAL_CONNECTIVITY = 24;
    public static final int NET_CAPABILITY_TEMPORARILY_NOT_METERED = 25;
    public static final int NET_CAPABILITY_OEM_PRIVATE = 26;
    public static final int NET_CAPABILITY_VEHICLE_INTERNAL = 27;
    public static final int NET_CAPABILITY_NOT_VCN_MANAGED = 28;
    public static final int NET_CAPABILITY_ENTERPRISE = 29;
    public static final int NET_CAPABILITY_VSIM = 30;
    public static final int NET_CAPABILITY_BIP = 31;
    public static final int NET_CAPABILITY_HEAD_UNIT = 32;
    public static final int NET_CAPABILITY_MMTEL = 33;
    public static final int NET_CAPABILITY_PRIORITIZE_LATENCY = 34;
    public static final int NET_CAPABILITY_PRIORITIZE_BANDWIDTH = 35;
    public static final int NET_CAPABILITY_LOCAL_NETWORK = 36;
    public static final int NET_CAPABILITY_NOT_BANDWIDTH_CONSTRAINED = 37;

    public static final int TRANSPORT_CELLULAR = 0;
    public static final int TRANSPORT_WIFI = 1;
    public static final int TRANSPORT_BLUETOOTH = 2;
    public static final int TRANSPORT_ETHERNET = 3;
    public static final int TRANSPORT_VPN = 4;
    public static final int TRANSPORT_WIFI_AWARE = 5;
    public static final int TRANSPORT_LOWPAN = 6;
    public static final int TRANSPORT_TEST = 7;
    public static final int TRANSPORT_USB = 8;
    public static final int TRANSPORT_THREAD = 9;
    public static final int TRANSPORT_SATELLITE = 10;

    public static final int NET_ENTERPRISE_ID_1 = 1;
    public static final int NET_ENTERPRISE_ID_2 = 2;
    public static final int NET_ENTERPRISE_ID_3 = 3;
    public static final int NET_ENTERPRISE_ID_4 = 4;
    public static final int NET_ENTERPRISE_ID_5 = 5;

    public static final int SIGNAL_STRENGTH_UNSPECIFIED = Integer.MIN_VALUE;

    private static final String[] TRANSPORT_NAMES = {"CELLULAR", "WIFI", "BLUETOOTH",
        "ETHERNET", "VPN", "WIFI_AWARE", "LOWPAN", "TEST", "USB", "THREAD", "SATELLITE"};

    /** As the module's: what a new NetworkCapabilities (and so a NetworkRequest) asks for. */
    private static final long DEFAULT_CAPABILITIES = (1L << NET_CAPABILITY_NOT_RESTRICTED)
            | (1L << NET_CAPABILITY_TRUSTED) | (1L << NET_CAPABILITY_NOT_VPN);

    private long mCapabilities;
    private long mTransports;
    private int mLinkUpBandwidthKbps;
    private int mLinkDownBandwidthKbps;
    private int mSignalStrength = SIGNAL_STRENGTH_UNSPECIFIED;
    private NetworkSpecifier mNetworkSpecifier;
    private TransportInfo mTransportInfo;
    private int mOwnerUid = android.os.Process.INVALID_UID;
    private List<Network> mUnderlyingNetworks;

    public NetworkCapabilities() {
        mCapabilities = DEFAULT_CAPABILITIES;
    }

    public NetworkCapabilities(NetworkCapabilities nc) {
        if (nc != null) set(nc);
    }

    /** @hide */
    public NetworkCapabilities(NetworkCapabilities nc, long redactions) {
        this(nc);
    }

    /** @hide */
    public void set(NetworkCapabilities nc) {
        mCapabilities = nc.mCapabilities;
        mTransports = nc.mTransports;
        mLinkUpBandwidthKbps = nc.mLinkUpBandwidthKbps;
        mLinkDownBandwidthKbps = nc.mLinkDownBandwidthKbps;
        mSignalStrength = nc.mSignalStrength;
        mNetworkSpecifier = nc.mNetworkSpecifier;
        mTransportInfo = nc.mTransportInfo;
        mOwnerUid = nc.mOwnerUid;
        mUnderlyingNetworks = nc.mUnderlyingNetworks;
    }

    /** @hide */
    public void clearAll() {
        mCapabilities = 0;
        mTransports = 0;
        mLinkUpBandwidthKbps = 0;
        mLinkDownBandwidthKbps = 0;
        mSignalStrength = SIGNAL_STRENGTH_UNSPECIFIED;
        mNetworkSpecifier = null;
        mTransportInfo = null;
        mUnderlyingNetworks = null;
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
    public NetworkCapabilities setCapability(int capability, boolean value) {
        return value ? addCapability(capability) : removeCapability(capability);
    }

    public boolean hasCapability(int capability) {
        return capability >= 0 && capability < 64 && (mCapabilities & (1L << capability)) != 0;
    }

    public int[] getCapabilities() {
        return bits(mCapabilities);
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

    public boolean hasTransport(int transportType) {
        return transportType >= 0 && transportType < 64
                && (mTransports & (1L << transportType)) != 0;
    }

    public int[] getTransportTypes() {
        return bits(mTransports);
    }

    public int[] getEnterpriseIds() {
        return new int[0];
    }

    public boolean hasEnterpriseId(int enterpriseId) {
        return false;
    }

    /** @hide */
    public NetworkCapabilities setLinkUpstreamBandwidthKbps(int upKbps) {
        mLinkUpBandwidthKbps = upKbps;
        return this;
    }

    public int getLinkUpstreamBandwidthKbps() {
        return mLinkUpBandwidthKbps;
    }

    /** @hide */
    public NetworkCapabilities setLinkDownstreamBandwidthKbps(int downKbps) {
        mLinkDownBandwidthKbps = downKbps;
        return this;
    }

    public int getLinkDownstreamBandwidthKbps() {
        return mLinkDownBandwidthKbps;
    }

    /** @hide */
    public NetworkCapabilities setNetworkSpecifier(NetworkSpecifier specifier) {
        mNetworkSpecifier = specifier;
        return this;
    }

    public NetworkSpecifier getNetworkSpecifier() {
        return mNetworkSpecifier;
    }

    /** @hide */
    public NetworkCapabilities setTransportInfo(TransportInfo info) {
        mTransportInfo = info;
        return this;
    }

    public TransportInfo getTransportInfo() {
        return mTransportInfo;
    }

    /** @hide */
    public NetworkCapabilities setSignalStrength(int signalStrength) {
        mSignalStrength = signalStrength;
        return this;
    }

    public int getSignalStrength() {
        return mSignalStrength;
    }

    /** @hide */
    public NetworkCapabilities setOwnerUid(int uid) {
        mOwnerUid = uid;
        return this;
    }

    public int getOwnerUid() {
        return mOwnerUid;
    }

    public int[] getAdministratorUids() {
        return new int[0];
    }

    /** @hide */
    public java.util.Set<Integer> getSubscriptionIds() {
        return Collections.emptySet();
    }

    /** @hide */
    public NetworkCapabilities setUnderlyingNetworks(List<Network> networks) {
        mUnderlyingNetworks = networks == null ? null : new ArrayList<>(networks);
        return this;
    }

    /** @hide */
    public List<Network> getUnderlyingNetworks() {
        return mUnderlyingNetworks == null ? null : Collections.unmodifiableList(mUnderlyingNetworks);
    }

    /**
     * Whether a network with these capabilities satisfies {@code request}'s: every
     * capability asked for, and one of its transports if it names any (as the module's
     * satisfiedByNetworkCapabilities, for what Gesher's networks have).
     * @hide
     */
    public boolean satisfiedByNetworkCapabilities(NetworkCapabilities nc) {
        if (nc == null) return false;
        if ((nc.mCapabilities & mCapabilities) != mCapabilities) return false;
        if (mTransports != 0 && (nc.mTransports & mTransports) == 0) return false;
        if (mLinkUpBandwidthKbps > nc.mLinkUpBandwidthKbps
                || mLinkDownBandwidthKbps > nc.mLinkDownBandwidthKbps) {
            return false;
        }
        if (mSignalStrength != SIGNAL_STRENGTH_UNSPECIFIED
                && mSignalStrength > nc.mSignalStrength) {
            return false;
        }
        return mNetworkSpecifier == null || mNetworkSpecifier instanceof MatchAllNetworkSpecifier
                || mNetworkSpecifier.equals(nc.mNetworkSpecifier);
    }

    private static int[] bits(long mask) {
        int[] out = new int[Long.bitCount(mask)];
        for (int i = 0, n = 0; i < 64; i++) {
            if ((mask & (1L << i)) != 0) out[n++] = i;
        }
        return out;
    }

    /** @hide */
    public static String capabilityNameOf(int capability) {
        switch (capability) {
            case NET_CAPABILITY_MMS: return "MMS";
            case NET_CAPABILITY_SUPL: return "SUPL";
            case NET_CAPABILITY_DUN: return "DUN";
            case NET_CAPABILITY_FOTA: return "FOTA";
            case NET_CAPABILITY_IMS: return "IMS";
            case NET_CAPABILITY_CBS: return "CBS";
            case NET_CAPABILITY_WIFI_P2P: return "WIFI_P2P";
            case NET_CAPABILITY_IA: return "IA";
            case NET_CAPABILITY_RCS: return "RCS";
            case NET_CAPABILITY_XCAP: return "XCAP";
            case NET_CAPABILITY_EIMS: return "EIMS";
            case NET_CAPABILITY_NOT_METERED: return "NOT_METERED";
            case NET_CAPABILITY_INTERNET: return "INTERNET";
            case NET_CAPABILITY_NOT_RESTRICTED: return "NOT_RESTRICTED";
            case NET_CAPABILITY_TRUSTED: return "TRUSTED";
            case NET_CAPABILITY_NOT_VPN: return "NOT_VPN";
            case NET_CAPABILITY_VALIDATED: return "VALIDATED";
            case NET_CAPABILITY_CAPTIVE_PORTAL: return "CAPTIVE_PORTAL";
            case NET_CAPABILITY_NOT_ROAMING: return "NOT_ROAMING";
            case NET_CAPABILITY_FOREGROUND: return "FOREGROUND";
            case NET_CAPABILITY_NOT_CONGESTED: return "NOT_CONGESTED";
            case NET_CAPABILITY_NOT_SUSPENDED: return "NOT_SUSPENDED";
            case NET_CAPABILITY_TEMPORARILY_NOT_METERED: return "TEMPORARILY_NOT_METERED";
            case NET_CAPABILITY_NOT_VCN_MANAGED: return "NOT_VCN_MANAGED";
            case NET_CAPABILITY_NOT_BANDWIDTH_CONSTRAINED: return "NOT_BANDWIDTH_CONSTRAINED";
            default: return Integer.toString(capability);
        }
    }

    /** @hide */
    public static String transportNameOf(int transport) {
        return transport >= 0 && transport < TRANSPORT_NAMES.length
                ? TRANSPORT_NAMES[transport] : "UNKNOWN";
    }

    @Override
    public String toString() {
        StringBuilder sb = new StringBuilder("[");
        int[] transports = getTransportTypes();
        if (transports.length > 0) {
            sb.append(" Transports: ");
            for (int i = 0; i < transports.length; i++) {
                sb.append(i > 0 ? "|" : "").append(transportNameOf(transports[i]));
            }
        }
        int[] caps = getCapabilities();
        if (caps.length > 0) {
            sb.append(" Capabilities: ");
            for (int i = 0; i < caps.length; i++) {
                sb.append(i > 0 ? "&" : "").append(capabilityNameOf(caps[i]));
            }
        }
        if (mLinkUpBandwidthKbps > 0) sb.append(" LinkUpBandwidth>=").append(mLinkUpBandwidthKbps).append("Kbps");
        if (mLinkDownBandwidthKbps > 0) sb.append(" LinkDnBandwidth>=").append(mLinkDownBandwidthKbps).append("Kbps");
        if (mNetworkSpecifier != null) sb.append(" Specifier: <").append(mNetworkSpecifier).append(">");
        return sb.append("]").toString();
    }

    @Override
    public boolean equals(Object o) {
        if (!(o instanceof NetworkCapabilities)) return false;
        NetworkCapabilities nc = (NetworkCapabilities) o;
        return mCapabilities == nc.mCapabilities && mTransports == nc.mTransports
                && mLinkUpBandwidthKbps == nc.mLinkUpBandwidthKbps
                && mLinkDownBandwidthKbps == nc.mLinkDownBandwidthKbps
                && mSignalStrength == nc.mSignalStrength && mOwnerUid == nc.mOwnerUid
                && Objects.equals(mNetworkSpecifier, nc.mNetworkSpecifier)
                && Objects.equals(mTransportInfo, nc.mTransportInfo)
                && Objects.equals(mUnderlyingNetworks, nc.mUnderlyingNetworks);
    }

    @Override
    public int hashCode() {
        return Objects.hash(mCapabilities, mTransports, mLinkUpBandwidthKbps,
                mLinkDownBandwidthKbps, mSignalStrength, mNetworkSpecifier, mOwnerUid);
    }

    @Override
    public int describeContents() {
        return 0;
    }

    @Override
    public void writeToParcel(Parcel dest, int flags) {
        dest.writeLong(mCapabilities);
        dest.writeLong(mTransports);
        dest.writeInt(mLinkUpBandwidthKbps);
        dest.writeInt(mLinkDownBandwidthKbps);
        dest.writeInt(mSignalStrength);
        dest.writeInt(mOwnerUid);
        dest.writeTypedList(mUnderlyingNetworks);
    }

    public static final Creator<NetworkCapabilities> CREATOR =
            new Creator<NetworkCapabilities>() {
                @Override
                public NetworkCapabilities createFromParcel(Parcel in) {
                    NetworkCapabilities nc = new NetworkCapabilities();
                    nc.mCapabilities = in.readLong();
                    nc.mTransports = in.readLong();
                    nc.mLinkUpBandwidthKbps = in.readInt();
                    nc.mLinkDownBandwidthKbps = in.readInt();
                    nc.mSignalStrength = in.readInt();
                    nc.mOwnerUid = in.readInt();
                    nc.mUnderlyingNetworks = in.createTypedArrayList(Network.CREATOR);
                    return nc;
                }

                @Override
                public NetworkCapabilities[] newArray(int size) {
                    return new NetworkCapabilities[size];
                }
            };
}
