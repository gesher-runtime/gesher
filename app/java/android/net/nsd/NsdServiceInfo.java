/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.net.nsd;

import android.net.Network;
import android.os.Parcel;
import android.os.Parcelable;
import android.util.ArrayMap;

import java.net.InetAddress;
import java.net.UnknownHostException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeSet;

/**
 * A DNS-SD service, as the Connectivity module's NsdServiceInfo describes one
 * (framework.jar compiles against the module's API and does not contain it): name, type,
 * subtypes, host addresses, port and TXT attributes. A plain value class; see
 * {@link NsdManager}.
 */
public final class NsdServiceInfo implements Parcelable {
    private String mServiceName;
    private String mServiceType;
    private final Set<String> mSubtypes = new TreeSet<>();
    private final ArrayMap<String, byte[]> mTxtRecord = new ArrayMap<>();
    private final List<InetAddress> mHostAddresses = new ArrayList<>();
    private String mHostname;
    private int mPort;
    private Network mNetwork;
    private int mInterfaceIndex;

    public NsdServiceInfo() {
    }

    public NsdServiceInfo(NsdServiceInfo other) {
        mServiceName = other.mServiceName;
        mServiceType = other.mServiceType;
        mSubtypes.addAll(other.mSubtypes);
        mTxtRecord.putAll(other.mTxtRecord);
        mHostAddresses.addAll(other.mHostAddresses);
        mHostname = other.mHostname;
        mPort = other.mPort;
        mNetwork = other.mNetwork;
        mInterfaceIndex = other.mInterfaceIndex;
    }

    public String getServiceName() {
        return mServiceName;
    }

    public void setServiceName(String s) {
        mServiceName = s;
    }

    public String getServiceType() {
        return mServiceType;
    }

    public void setServiceType(String s) {
        mServiceType = s;
    }

    public Set<String> getSubtypes() {
        return Collections.unmodifiableSet(mSubtypes);
    }

    public void setSubtypes(Set<String> subtypes) {
        mSubtypes.clear();
        mSubtypes.addAll(subtypes);
    }

    @Deprecated
    public InetAddress getHost() {
        return mHostAddresses.isEmpty() ? null : mHostAddresses.get(0);
    }

    @Deprecated
    public void setHost(InetAddress s) {
        setHostAddresses(s == null ? Collections.emptyList() : Collections.singletonList(s));
    }

    public List<InetAddress> getHostAddresses() {
        return new ArrayList<>(mHostAddresses);
    }

    public void setHostAddresses(List<InetAddress> addresses) {
        mHostAddresses.clear();
        mHostAddresses.addAll(addresses);
    }

    public String getHostname() {
        return mHostname;
    }

    public void setHostname(String hostname) {
        mHostname = hostname;
    }

    public int getPort() {
        return mPort;
    }

    public void setPort(int p) {
        mPort = p;
    }

    public Network getNetwork() {
        return mNetwork;
    }

    public void setNetwork(Network network) {
        mNetwork = network;
    }

    public int getInterfaceIndex() {
        return mInterfaceIndex;
    }

    /** A TXT attribute: the key is printable ASCII without '=', 1 to 9 bytes with its value. */
    public void setAttribute(String key, String value) {
        setAttribute(key, value == null ? null : value.getBytes(StandardCharsets.UTF_8));
    }

    public void setAttribute(String key, byte[] value) {
        if (key == null || key.isEmpty() || key.indexOf('=') >= 0) {
            throw new IllegalArgumentException("Invalid key: " + key);
        }
        for (int i = 0; i < key.length(); i++) {
            char c = key.charAt(i);
            if (c < 0x20 || c > 0x7e) throw new IllegalArgumentException("Invalid key: " + key);
        }
        mTxtRecord.put(key, value);
    }

    public void removeAttribute(String key) {
        mTxtRecord.remove(key);
    }

    public Map<String, byte[]> getAttributes() {
        return Collections.unmodifiableMap(mTxtRecord);
    }

    @Override
    public String toString() {
        return "name: " + mServiceName + ", type: " + mServiceType + ", subtypes: " + mSubtypes
                + ", hostAddresses: " + mHostAddresses + ", hostname: " + mHostname
                + ", port: " + mPort + ", network: " + mNetwork;
    }

    @Override
    public int describeContents() {
        return 0;
    }

    @Override
    public void writeToParcel(Parcel dest, int flags) {
        dest.writeString(mServiceName);
        dest.writeString(mServiceType);
        dest.writeStringList(new ArrayList<>(mSubtypes));
        dest.writeInt(mTxtRecord.size());
        for (int i = 0; i < mTxtRecord.size(); i++) {
            dest.writeString(mTxtRecord.keyAt(i));
            dest.writeByteArray(mTxtRecord.valueAt(i));
        }
        dest.writeInt(mHostAddresses.size());
        for (InetAddress a : mHostAddresses) dest.writeByteArray(a.getAddress());
        dest.writeString(mHostname);
        dest.writeInt(mPort);
        dest.writeParcelable(mNetwork, flags);
        dest.writeInt(mInterfaceIndex);
    }

    public static final Creator<NsdServiceInfo> CREATOR = new Creator<NsdServiceInfo>() {
        @Override
        public NsdServiceInfo createFromParcel(Parcel in) {
            NsdServiceInfo info = new NsdServiceInfo();
            info.mServiceName = in.readString();
            info.mServiceType = in.readString();
            List<String> subtypes = new ArrayList<>();
            in.readStringList(subtypes);
            info.mSubtypes.addAll(subtypes);
            for (int i = in.readInt(); i > 0; i--) {
                info.mTxtRecord.put(in.readString(), in.createByteArray());
            }
            for (int i = in.readInt(); i > 0; i--) {
                try {
                    info.mHostAddresses.add(InetAddress.getByAddress(in.createByteArray()));
                } catch (UnknownHostException e) {
                    // Not 4 or 16 bytes: a parcel this class did not write.
                }
            }
            info.mHostname = in.readString();
            info.mPort = in.readInt();
            info.mNetwork = in.readParcelable(Network.class.getClassLoader(), Network.class);
            info.mInterfaceIndex = in.readInt();
            return info;
        }

        @Override
        public NsdServiceInfo[] newArray(int size) {
            return new NsdServiceInfo[size];
        }
    };
}
