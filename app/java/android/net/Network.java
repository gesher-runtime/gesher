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
import java.net.InetSocketAddress;
import java.net.Proxy;
import com.gesher.net.WindowsNetworks;

import javax.net.SocketFactory;

/**
 * The Connectivity module's Network (framework.jar compiles against the module's API and
 * does not contain it), with its public API (docs/network.md). A network is one of
 * Windows' (its netId is 100 + the interface index, {@link ConnectivityManager}); Windows
 * binds sockets to its interface using Windows' outgoing-interface options and
 * resolves names through that interface's DNS servers.
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
        return WindowsNetworks.resolve(netId, host);
    }

    public InetAddress getByName(String host) throws UnknownHostException {
        return getAllByName(host)[0];
    }

    public SocketFactory getSocketFactory() {
        return new SocketFactory() {
            @Override
            public Socket createSocket() throws IOException {
                Socket socket = new Socket(Proxy.NO_PROXY);
                try {
                    bindSocket(socket);
                    return socket;
                } catch (IOException | RuntimeException e) {
                    socket.close();
                    throw e;
                }
            }

            private Socket connect(InetAddress[] addresses, int port, InetAddress local,
                                   int localPort) throws IOException {
                IOException failure = null;
                for (InetAddress address : addresses) {
                    Socket socket = createSocket();
                    try {
                        if (local != null) socket.bind(new InetSocketAddress(local, localPort));
                        socket.connect(new InetSocketAddress(address, port));
                        return socket;
                    } catch (IOException e) {
                        failure = e;
                        socket.close();
                    }
                }
                throw failure != null ? failure : new UnknownHostException("No addresses");
            }

            @Override
            public Socket createSocket(String host, int port) throws IOException {
                return connect(getAllByName(host), port, null, 0);
            }

            @Override
            public Socket createSocket(String host, int port, InetAddress local, int localPort)
                    throws IOException {
                return connect(getAllByName(host), port, local, localPort);
            }

            @Override
            public Socket createSocket(InetAddress host, int port) throws IOException {
                return connect(new InetAddress[] {host}, port, null, 0);
            }

            @Override
            public Socket createSocket(InetAddress host, int port, InetAddress local, int localPort)
                    throws IOException {
                return connect(new InetAddress[] {host}, port, local, localPort);
            }
        };
    }

    public URLConnection openConnection(URL url) throws IOException {
        return openConnectionInternal(url, null);
    }

    public URLConnection openConnection(URL url, java.net.Proxy proxy) throws IOException {
        if (proxy == null) throw new IllegalArgumentException("proxy is null");
        return openConnectionInternal(url, proxy);
    }

    private URLConnection openConnectionInternal(URL url, Proxy proxy) throws IOException {
        // The Connectivity module uses this same OkHttp internal Android API.
        // Reflect because the boot compilation image has no OkHttp dependency;
        // its real implementation is included in core-libart at runtime.
        try {
            Class<?> factoryClass = Class.forName(
                    "com.android.okhttp.internalandroidapi.HttpURLConnectionFactory");
            Class<?> dnsClass = Class.forName("com.android.okhttp.internalandroidapi.Dns");
            Object factory = factoryClass.getConstructor().newInstance();
            Object dns = java.lang.reflect.Proxy.newProxyInstance(dnsClass.getClassLoader(),
                    new Class<?>[] {dnsClass}, (object, method, args) -> {
                        if (method.getName().equals("lookup")) {
                            return java.util.Arrays.asList(getAllByName((String) args[0]));
                        }
                        if (method.getName().equals("hashCode")) return netId;
                        if (method.getName().equals("equals")) return object == args[0];
                        return "Network DNS " + netId;
                    });
            factoryClass.getMethod("setDns", dnsClass).invoke(factory, dns);
            factoryClass.getMethod("setNewConnectionPool", int.class, long.class,
                    java.util.concurrent.TimeUnit.class).invoke(factory, 5, 5L,
                            java.util.concurrent.TimeUnit.MINUTES);
            if (proxy == null) {
                return (URLConnection) factoryClass.getMethod("openConnection", URL.class,
                        SocketFactory.class).invoke(factory, url, getSocketFactory());
            }
            return (URLConnection) factoryClass.getMethod("openConnection", URL.class,
                    SocketFactory.class, Proxy.class).invoke(factory, url, getSocketFactory(), proxy);
        } catch (java.lang.reflect.InvocationTargetException e) {
            Throwable cause = e.getCause();
            if (cause instanceof IOException) throw (IOException) cause;
            if (cause instanceof RuntimeException) throw (RuntimeException) cause;
            throw new IOException("Cannot open connection on network " + netId, cause);
        } catch (ReflectiveOperationException e) {
            throw new IOException("Android HTTP connection factory unavailable", e);
        }
    }

    public void bindSocket(DatagramSocket socket) throws IOException {
        if (socket.isConnected()) throw new java.net.SocketException("Socket is connected");
        socket.getReuseAddress();  // Creates the native endpoint if needed.
        bindSocket(socket.getFileDescriptor$());
    }

    public void bindSocket(Socket socket) throws IOException {
        if (socket.isConnected()) throw new java.net.SocketException("Socket is connected");
        socket.getReuseAddress();
        bindSocket(socket.getFileDescriptor$());
    }

    public void bindSocket(FileDescriptor fd) throws IOException {
        WindowsNetworks.bindSocket(fd, netId);
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
