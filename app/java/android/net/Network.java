/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.net;

import android.os.Parcel;
import android.os.Parcelable;

import java.io.FileDescriptor;
import java.io.IOException;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.Socket;
import java.net.URL;
import java.net.URLConnection;
import java.net.UnknownHostException;

import javax.net.SocketFactory;

/**
 * The Connectivity module's Network (framework.jar compiles against the module's API and
 * does not contain it), with its public API (docs/network.md). A network is one of
 * Windows' (its netId is 100 + the interface index, {@link ConnectivityManager}); Windows
 * picks the route of each connection, so binding a socket or a process to a network is
 * accepted and leaves the route to Windows.
 */
public class Network implements Parcelable {
    /** As the module's HANDLE_MAGIC. */
    private static final long HANDLE_MAGIC = 0xcafed00dL;
    private static final int HANDLE_MAGIC_SIZE = 32;

    /** @hide */
    public final int netId;

    /** @hide */
    public Network(int netId) {
        this.netId = netId;
    }

    /** @hide */
    public Network(Network that) {
        this(that.netId);
    }

    /** @hide */
    public int getNetId() {
        return netId;
    }

    /** @hide */
    public Network getPrivateDnsBypassingCopy() {
        return this;
    }

    public InetAddress[] getAllByName(String host) throws UnknownHostException {
        return InetAddress.getAllByName(host);
    }

    public InetAddress getByName(String host) throws UnknownHostException {
        return InetAddress.getByName(host);
    }

    public SocketFactory getSocketFactory() {
        return SocketFactory.getDefault();
    }

    public URLConnection openConnection(URL url) throws IOException {
        return url.openConnection();
    }

    public URLConnection openConnection(URL url, java.net.Proxy proxy) throws IOException {
        if (proxy == null) throw new IllegalArgumentException("proxy is null");
        return url.openConnection(proxy);
    }

    public void bindSocket(DatagramSocket socket) throws IOException {
        if (socket.isConnected()) throw new java.net.SocketException("Socket is connected");
    }

    public void bindSocket(Socket socket) throws IOException {
        if (socket.isConnected()) throw new java.net.SocketException("Socket is connected");
    }

    public void bindSocket(FileDescriptor fd) throws IOException {
    }

    public long getNetworkHandle() {
        return netId == 0 ? 0L : (((long) netId) << HANDLE_MAGIC_SIZE) | HANDLE_MAGIC;
    }

    public static Network fromNetworkHandle(long networkHandle) {
        if (networkHandle == 0) {
            throw new IllegalArgumentException("Network.fromNetworkHandle refusing to "
                    + "instantiate NETID_UNSET Network.");
        }
        if ((networkHandle & ((1L << HANDLE_MAGIC_SIZE) - 1)) != HANDLE_MAGIC) {
            throw new IllegalArgumentException("Value passed to fromNetworkHandle() is not a "
                    + "network handle.");
        }
        return new Network((int) (networkHandle >>> HANDLE_MAGIC_SIZE));
    }

    @Override
    public boolean equals(Object o) {
        return o instanceof Network && ((Network) o).netId == netId;
    }

    @Override
    public int hashCode() {
        return netId * 11;
    }

    @Override
    public String toString() {
        return Integer.toString(netId);
    }

    @Override
    public int describeContents() {
        return 0;
    }

    @Override
    public void writeToParcel(Parcel dest, int flags) {
        dest.writeInt(netId);
    }

    public static final Creator<Network> CREATOR = new Creator<Network>() {
        @Override
        public Network createFromParcel(Parcel in) {
            return new Network(in.readInt());
        }

        @Override
        public Network[] newArray(int size) {
            return new Network[size];
        }
    };
}
