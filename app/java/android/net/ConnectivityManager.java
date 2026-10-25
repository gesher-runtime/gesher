/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.net;

import android.app.PendingIntent;
import android.content.Context;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.RemoteException;
import android.os.ServiceManager;

import com.gesher.net.IConnectivity;
import com.gesher.net.WindowsNetworks;

import java.net.InetAddress;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * ConnectivityManager on Windows (docs/network.md, ADR 0037), with the Connectivity
 * module's public API (framework.jar compiles against the module's API and does not
 * contain it). It is the client of the {@code connectivity} service
 * ({@code com.gesher.server.ConnectivityService}, {@link IConnectivity}), which describes
 * Windows' networks: each up adapter that reaches beyond its link is a {@link Network}
 * (netId 100 + its interface index) with {@link NetworkCapabilities} (Wi-Fi, Ethernet,
 * cellular or VPN; validated, metered, roaming from the Network List Manager),
 * {@link LinkProperties} (interface, addresses, routes, DNS) and a legacy
 * {@link NetworkInfo}; the active network is the one Windows routes the internet
 * through. Network callbacks are called on their handler (or ConnectivityThread) when the
 * service reports a change. ACCESS_NETWORK_STATE is checked by the service.
 *
 * <p>Registered as {@code connectivity} by {@link ConnectivityFrameworkInitializer}.
 * Without the service (an older system server), it answers like a device with no network.
 */
public class ConnectivityManager {
    public static final String CONNECTIVITY_ACTION = "android.net.conn.CONNECTIVITY_CHANGE";
    public static final String ACTION_CAPTIVE_PORTAL_SIGN_IN =
            "android.net.conn.CAPTIVE_PORTAL";
    public static final String ACTION_RESTRICT_BACKGROUND_CHANGED =
            "android.net.conn.RESTRICT_BACKGROUND_CHANGED";
    @Deprecated
    public static final String ACTION_BACKGROUND_DATA_SETTING_CHANGED =
            "android.net.conn.BACKGROUND_DATA_SETTING_CHANGED";
    public static final String EXTRA_NETWORK_INFO = "networkInfo";
    public static final String EXTRA_NETWORK_TYPE = "networkType";
    public static final String EXTRA_NO_CONNECTIVITY = "noConnectivity";
    public static final String EXTRA_IS_FAILOVER = "isFailover";
    public static final String EXTRA_OTHER_NETWORK_INFO = "otherNetwork";
    public static final String EXTRA_REASON = "reason";
    public static final String EXTRA_EXTRA_INFO = "extraInfo";
    public static final String EXTRA_NETWORK = "android.net.extra.NETWORK";
    public static final String EXTRA_NETWORK_REQUEST = "android.net.extra.NETWORK_REQUEST";
    public static final String EXTRA_CAPTIVE_PORTAL = "android.net.extra.CAPTIVE_PORTAL";
    public static final String EXTRA_CAPTIVE_PORTAL_URL = "android.net.extra.CAPTIVE_PORTAL_URL";
    public static final int TYPE_NONE = -1;
    public static final int TYPE_MOBILE = 0;
    public static final int TYPE_WIFI = 1;
    public static final int TYPE_MOBILE_MMS = 2;
    public static final int TYPE_MOBILE_SUPL = 3;
    public static final int TYPE_MOBILE_DUN = 4;
    public static final int TYPE_MOBILE_HIPRI = 5;
    public static final int TYPE_WIMAX = 6;
    public static final int TYPE_BLUETOOTH = 7;
    public static final int TYPE_DUMMY = 8;
    public static final int TYPE_ETHERNET = 9;
    public static final int TYPE_VPN = 17;
    public static final int DEFAULT_NETWORK_PREFERENCE = TYPE_WIFI;
    public static final int RESTRICT_BACKGROUND_STATUS_DISABLED = 1;
    public static final int RESTRICT_BACKGROUND_STATUS_WHITELISTED = 2;
    public static final int RESTRICT_BACKGROUND_STATUS_ENABLED = 3;
    public static final int MULTIPATH_PREFERENCE_HANDOVER = 1;
    public static final int MULTIPATH_PREFERENCE_RELIABILITY = 1 << 1;
    public static final int MULTIPATH_PREFERENCE_PERFORMANCE = 1 << 2;
    public static final int MULTIPATH_PREFERENCE_UNMETERED = MULTIPATH_PREFERENCE_HANDOVER
            | MULTIPATH_PREFERENCE_RELIABILITY | MULTIPATH_PREFERENCE_PERFORMANCE;

    private static volatile Network sProcessNetwork;

    private final Context mContext;

    public ConnectivityManager(Context context) {
        mContext = context;
    }

    /** As the module's hidden {@code from}. */
    public static ConnectivityManager from(Context context) {
        return context.getSystemService(ConnectivityManager.class);
    }

    public static boolean isNetworkTypeValid(int networkType) {
        return networkType >= TYPE_MOBILE && networkType <= TYPE_VPN;
    }

    // --- the service and its snapshots -------------------------------------------

    private static final Object sLock = new Object();
    private static IConnectivity sService;
    /** The latest snapshot while a listener keeps it current; null otherwise. */
    private static WindowsNetworks.Snapshot sCurrent;
    private static Listener sListener;
    private static final Map<NetworkCallback, CallbackRecord> sCallbacks = new LinkedHashMap<>();
    private static Handler sConnectivityHandler;

    private static IConnectivity service() {
        synchronized (sLock) {
            if (sService == null) {
                IBinder b = ServiceManager.getService(Context.CONNECTIVITY_SERVICE);
                sService = b != null ? IConnectivity.Stub.asInterface(b) : null;
            }
            return sService;
        }
    }

    private static final WindowsNetworks.Snapshot NO_NETWORK =
            WindowsNetworks.parse("internet\t0\t1\n");

    /** Windows' networks now (SecurityException without ACCESS_NETWORK_STATE). */
    private static WindowsNetworks.Snapshot snapshot() {
        synchronized (sLock) {
            if (sCurrent != null) return sCurrent;
        }
        IConnectivity service = service();
        if (service == null) return NO_NETWORK;
        try {
            return WindowsNetworks.parse(service.getSnapshot());
        } catch (RemoteException e) {
            throw e.rethrowFromSystemServer();
        }
    }

    private static Handler connectivityHandler() {
        synchronized (sLock) {
            if (sConnectivityHandler == null) {
                HandlerThread t = new HandlerThread("ConnectivityThread");
                t.start();
                sConnectivityHandler = new Handler(t.getLooper());
            }
            return sConnectivityHandler;
        }
    }

    /** Receives the service's snapshots while callbacks are registered. */
    private static final class Listener extends IConnectivity.IListener.Stub {
        @Override
        public void onSnapshot(String text) {
            WindowsNetworks.Snapshot s = WindowsNetworks.parse(text);
            List<CallbackRecord> records;
            synchronized (sLock) {
                if (sListener != this) return;
                sCurrent = s;
                records = new ArrayList<>(sCallbacks.values());
            }
            for (CallbackRecord r : records) r.post(s);
        }
    }

    // --- Windows' networks as Android's -------------------------------------------

    private static String interfaceName(WindowsNetworks.Net n) {
        try {
            String name = android.system.Os.if_indextoname(n.ifIndex);
            if (name != null && !name.isEmpty()) return name;
        } catch (Throwable t) {
            // No name gesher-base knows: the index will do.
        }
        return "if" + n.ifIndex;
    }

    private static int legacyType(WindowsNetworks.Net n) {
        switch (n.kind) {
            case "wifi": return TYPE_WIFI;
            case "cellular": return TYPE_MOBILE;
            case "vpn": return TYPE_VPN;
            default: return TYPE_ETHERNET;
        }
    }

    private static String legacyTypeName(int type) {
        switch (type) {
            case TYPE_WIFI: return "WIFI";
            case TYPE_MOBILE: return "MOBILE";
            case TYPE_VPN: return "VPN";
            default: return "ETHERNET";
        }
    }

    static NetworkCapabilities capabilitiesOf(WindowsNetworks.Snapshot s, WindowsNetworks.Net n) {
        NetworkCapabilities nc = new NetworkCapabilities();
        nc.clearAll();
        switch (n.kind) {
            case "wifi": nc.addTransportType(NetworkCapabilities.TRANSPORT_WIFI); break;
            case "cellular": nc.addTransportType(NetworkCapabilities.TRANSPORT_CELLULAR); break;
            case "vpn": nc.addTransportType(NetworkCapabilities.TRANSPORT_VPN); break;
            default: nc.addTransportType(NetworkCapabilities.TRANSPORT_ETHERNET); break;
        }
        boolean vpn = n.kind.equals("vpn");
        nc.addCapability(NetworkCapabilities.NET_CAPABILITY_NOT_RESTRICTED)
                .addCapability(NetworkCapabilities.NET_CAPABILITY_TRUSTED)
                .addCapability(NetworkCapabilities.NET_CAPABILITY_NOT_SUSPENDED)
                .addCapability(NetworkCapabilities.NET_CAPABILITY_FOREGROUND)
                .addCapability(NetworkCapabilities.NET_CAPABILITY_NOT_VCN_MANAGED)
                .addCapability(NetworkCapabilities.NET_CAPABILITY_NOT_BANDWIDTH_CONSTRAINED)
                .setCapability(NetworkCapabilities.NET_CAPABILITY_NOT_VPN, !vpn)
                .setCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET,
                        !n.gateways.isEmpty() || n.isDefault || vpn)
                .setCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED, n.internet)
                .setCapability(NetworkCapabilities.NET_CAPABILITY_NOT_METERED, !n.metered)
                .setCapability(NetworkCapabilities.NET_CAPABILITY_NOT_ROAMING, !n.roaming)
                .setCapability(NetworkCapabilities.NET_CAPABILITY_NOT_CONGESTED, !n.congested);
        nc.setLinkDownstreamBandwidthKbps((int) Math.min(Integer.MAX_VALUE, n.receiveBps / 1000));
        nc.setLinkUpstreamBandwidthKbps((int) Math.min(Integer.MAX_VALUE, n.transmitBps / 1000));
        if (vpn) {
            List<Network> underlying = new ArrayList<>();
            for (WindowsNetworks.Net other : s.networks) {
                if (other != n && !other.kind.equals("vpn") && !other.gateways.isEmpty()) {
                    underlying.add(new Network(other.netId()));
                }
            }
            nc.setUnderlyingNetworks(underlying);
        }
        return nc;
    }

    static LinkProperties linkPropertiesOf(WindowsNetworks.Net n) {
        LinkProperties lp = new LinkProperties();
        String iface = interfaceName(n);
        lp.setInterfaceName(iface);
        for (int i = 0; i < n.addresses.size(); i++) {
            InetAddress a = n.addresses.get(i);
            lp.addLinkAddress(new LinkAddress(a, n.prefixLengths.get(i)));
            // The address's own subnet, on the link.
            lp.addRoute(new RouteInfo(new IpPrefix(a, n.prefixLengths.get(i)), null, iface,
                    RouteInfo.RTN_UNICAST));
        }
        for (InetAddress gw : n.gateways) {
            byte[] any = new byte[gw.getAddress().length];
            lp.addRoute(new RouteInfo(new IpPrefix(any, 0), gw, iface, RouteInfo.RTN_UNICAST));
        }
        lp.setDnsServers(n.dnsServers);
        if (n.dnsSuffix != null && !n.dnsSuffix.isEmpty()) lp.setDomains(n.dnsSuffix);
        lp.setMtu(n.mtu);
        return lp;
    }

    static NetworkInfo networkInfoOf(WindowsNetworks.Net n) {
        int type = legacyType(n);
        NetworkInfo info = new NetworkInfo(type, 0, legacyTypeName(type), "");
        info.setDetailedState(NetworkInfo.DetailedState.CONNECTED, null, null);
        info.setIsAvailable(true);
        info.setRoaming(n.roaming);
        return info;
    }

    // --- queries ---------------------------------------------------------------

    public Network getActiveNetwork() {
        WindowsNetworks.Net n = snapshot().defaultNetwork();
        return n != null ? new Network(n.netId()) : null;
    }

    /** @hide */
    public Network getActiveNetworkForUid(int uid) {
        return getActiveNetwork();
    }

    @Deprecated
    public NetworkInfo getActiveNetworkInfo() {
        WindowsNetworks.Net n = snapshot().defaultNetwork();
        return n != null ? networkInfoOf(n) : null;
    }

    public Network[] getAllNetworks() {
        List<WindowsNetworks.Net> nets = snapshot().networks;
        Network[] out = new Network[nets.size()];
        for (int i = 0; i < out.length; i++) out[i] = new Network(nets.get(i).netId());
        return out;
    }

    @Deprecated
    public NetworkInfo[] getAllNetworkInfo() {
        List<WindowsNetworks.Net> nets = snapshot().networks;
        NetworkInfo[] out = new NetworkInfo[nets.size()];
        for (int i = 0; i < out.length; i++) out[i] = networkInfoOf(nets.get(i));
        return out;
    }

    @Deprecated
    public NetworkInfo getNetworkInfo(int networkType) {
        WindowsNetworks.Snapshot s = snapshot();
        WindowsNetworks.Net best = s.defaultNetwork();
        if (best != null && legacyType(best) == networkType) return networkInfoOf(best);
        for (WindowsNetworks.Net n : s.networks) {
            if (legacyType(n) == networkType) return networkInfoOf(n);
        }
        return null;
    }

    @Deprecated
    public NetworkInfo getNetworkInfo(Network network) {
        if (network == null) return null;
        WindowsNetworks.Net n = snapshot().byNetId(network.netId);
        return n != null ? networkInfoOf(n) : null;
    }

    public NetworkCapabilities getNetworkCapabilities(Network network) {
        if (network == null) return null;
        WindowsNetworks.Snapshot s = snapshot();
        WindowsNetworks.Net n = s.byNetId(network.netId);
        return n != null ? capabilitiesOf(s, n) : null;
    }

    public LinkProperties getLinkProperties(Network network) {
        if (network == null) return null;
        WindowsNetworks.Net n = snapshot().byNetId(network.netId);
        return n != null ? linkPropertiesOf(n) : null;
    }

    @Deprecated
    public int getNetworkPreference() {
        return TYPE_NONE;
    }

    /** Metered when the active network costs (or there is none), as ConnectivityService. */
    public boolean isActiveNetworkMetered() {
        WindowsNetworks.Net n = snapshot().defaultNetwork();
        return n == null || n.metered;
    }

    public int getMultipathPreference(Network network) {
        WindowsNetworks.Snapshot s = snapshot();
        WindowsNetworks.Net n = network != null ? s.byNetId(network.netId) : s.defaultNetwork();
        return n != null && !n.metered ? MULTIPATH_PREFERENCE_UNMETERED : 0;
    }

    public int getRestrictBackgroundStatus() {
        return RESTRICT_BACKGROUND_STATUS_DISABLED;
    }

    @Deprecated
    public boolean getBackgroundDataSetting() {
        return true;
    }

    public boolean isDefaultNetworkActive() {
        return snapshot().defaultNetwork() != null;
    }

    public void addDefaultNetworkActiveListener(OnNetworkActiveListener l) {
    }

    public void removeDefaultNetworkActiveListener(OnNetworkActiveListener l) {
    }

    /** Windows' proxy is not carried over: none, as on a device without one. */
    public ProxyInfo getDefaultProxy() {
        return null;
    }

    /** @hide */
    public ProxyInfo getProxyForNetwork(Network network) {
        return null;
    }

    /** @hide */
    public ProxyInfo getGlobalProxy() {
        return null;
    }

    public int getConnectionOwnerUid(int protocol, java.net.InetSocketAddress local,
            java.net.InetSocketAddress remote) {
        return android.os.Process.INVALID_UID;
    }

    public void reportNetworkConnectivity(Network network, boolean hasConnectivity) {
    }

    @Deprecated
    public void reportBadNetwork(Network network) {
    }

    public boolean requestBandwidthUpdate(Network network) {
        return network != null && snapshot().byNetId(network.netId) != null;
    }

    public boolean bindProcessToNetwork(Network network) {
        sProcessNetwork = network;
        return true;
    }

    public Network getBoundNetworkForProcess() {
        return sProcessNetwork;
    }

    @Deprecated
    public static boolean setProcessDefaultNetwork(Network network) {
        sProcessNetwork = network;
        return true;
    }

    @Deprecated
    public static Network getProcessDefaultNetwork() {
        return sProcessNetwork;
    }

    // --- callbacks ---------------------------------------------------------------

    public void registerNetworkCallback(NetworkRequest request, NetworkCallback networkCallback) {
        registerNetworkCallback(request, networkCallback, null);
    }

    public void registerNetworkCallback(NetworkRequest request, NetworkCallback networkCallback,
            Handler handler) {
        register(request, networkCallback, handler, Kind.LISTEN, 0);
    }

    public void registerNetworkCallback(NetworkRequest request, PendingIntent operation) {
        snapshot();  // the permission check
    }

    public void registerDefaultNetworkCallback(NetworkCallback networkCallback) {
        registerDefaultNetworkCallback(networkCallback, null);
    }

    public void registerDefaultNetworkCallback(NetworkCallback networkCallback,
            Handler handler) {
        register(null, networkCallback, handler, Kind.DEFAULT, 0);
    }

    /** @hide */
    public void registerSystemDefaultNetworkCallback(NetworkCallback networkCallback,
            Handler handler) {
        register(null, networkCallback, handler, Kind.DEFAULT, 0);
    }

    public void registerBestMatchingNetworkCallback(NetworkRequest request,
            NetworkCallback networkCallback, Handler handler) {
        register(request, networkCallback, handler, Kind.BEST, 0);
    }

    public void requestNetwork(NetworkRequest request, NetworkCallback networkCallback) {
        requestNetwork(request, networkCallback, null);
    }

    public void requestNetwork(NetworkRequest request, NetworkCallback networkCallback,
            Handler handler) {
        register(request, networkCallback, handler, Kind.BEST, 0);
    }

    public void requestNetwork(NetworkRequest request, NetworkCallback networkCallback,
            int timeoutMs) {
        requestNetwork(request, networkCallback, null, timeoutMs);
    }

    public void requestNetwork(NetworkRequest request, NetworkCallback networkCallback,
            Handler handler, int timeoutMs) {
        if (timeoutMs <= 0) throw new IllegalArgumentException("Non-positive timeoutMs: " + timeoutMs);
        register(request, networkCallback, handler, Kind.BEST, timeoutMs);
    }

    public void requestNetwork(NetworkRequest request, PendingIntent operation) {
        snapshot();
    }

    public void releaseNetworkRequest(PendingIntent operation) {
    }

    public void unregisterNetworkCallback(PendingIntent operation) {
    }

    public void unregisterNetworkCallback(NetworkCallback networkCallback) {
        if (networkCallback == null) throw new IllegalArgumentException("null NetworkCallback");
        CallbackRecord r;
        synchronized (sLock) {
            r = sCallbacks.remove(networkCallback);
            if (r == null) {
                throw new IllegalArgumentException("NetworkCallback was not registered");
            }
            r.active = false;
            if (sCallbacks.isEmpty() && sListener != null) {
                Listener l = sListener;
                sListener = null;
                sCurrent = null;
                IConnectivity service = sService;
                try {
                    if (service != null) service.removeListener(l);
                } catch (RemoteException e) {
                    // The service is gone, and its listeners with it.
                }
            }
        }
    }

    private enum Kind { LISTEN, DEFAULT, BEST }

    private void register(NetworkRequest request, NetworkCallback cb, Handler handler, Kind kind,
            int timeoutMs) {
        if (cb == null) throw new IllegalArgumentException("null NetworkCallback");
        if (kind != Kind.DEFAULT && request == null) {
            throw new IllegalArgumentException("null NetworkRequest");
        }
        IConnectivity service = service();
        WindowsNetworks.Snapshot first;
        CallbackRecord r = new CallbackRecord(request, cb,
                handler != null ? handler : connectivityHandler(), kind);
        synchronized (sLock) {
            if (sCallbacks.containsKey(cb)) {
                throw new IllegalArgumentException("NetworkCallback was already registered");
            }
            if (service != null && sListener == null) {
                Listener l = new Listener();
                try {
                    // Checks ACCESS_NETWORK_STATE: a SecurityException leaves nothing registered.
                    sCurrent = WindowsNetworks.parse(service.getSnapshot());
                    service.addListener(l);
                } catch (RemoteException e) {
                    sCurrent = null;
                    throw e.rethrowFromSystemServer();
                }
                sListener = l;
            }
            sCallbacks.put(cb, r);
            first = sCurrent != null ? sCurrent : NO_NETWORK;
            r.post(first);  // before any later snapshot the listener posts
        }
        if (timeoutMs > 0) {
            r.handler.postDelayed(() -> {
                boolean unavailable;
                synchronized (sLock) {
                    unavailable = r.active && r.current.isEmpty() && !r.everAvailable;
                }
                if (unavailable) {
                    try {
                        unregisterNetworkCallback(cb);
                    } catch (IllegalArgumentException e) {
                        return;
                    }
                    cb.onUnavailable();
                }
            }, timeoutMs);
        }
    }

    /** A registered callback and the networks it was told of. */
    private static final class CallbackRecord {
        final NetworkRequest request;
        final NetworkCallback callback;
        final Handler handler;
        final Kind kind;
        volatile boolean active = true;
        boolean everAvailable;
        /** netId -> what the callback was last told of it. */
        final Map<Integer, NetworkCapabilities> current = new LinkedHashMap<>();
        final Map<Integer, LinkProperties> currentLp = new HashMap<>();

        CallbackRecord(NetworkRequest request, NetworkCallback callback, Handler handler,
                Kind kind) {
            this.request = request;
            this.callback = callback;
            this.handler = handler;
            this.kind = kind;
        }

        void post(WindowsNetworks.Snapshot s) {
            handler.post(() -> apply(s));
        }

        private boolean matches(NetworkCapabilities nc) {
            return request == null ? nc.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                    : request.canBeSatisfiedBy(nc);
        }

        /** Calls the callback for what changed since the last snapshot it saw. */
        void apply(WindowsNetworks.Snapshot s) {
            if (!active) return;
            Map<Integer, WindowsNetworks.Net> targets = new LinkedHashMap<>();
            Map<Integer, NetworkCapabilities> caps = new HashMap<>();
            WindowsNetworks.Net def = s.defaultNetwork();
            if (kind == Kind.LISTEN) {
                for (WindowsNetworks.Net n : s.networks) {
                    NetworkCapabilities nc = capabilitiesOf(s, n);
                    if (matches(nc)) {
                        targets.put(n.netId(), n);
                        caps.put(n.netId(), nc);
                    }
                }
            } else {
                // The default network, or for a request the best match: the default network
                // if it satisfies it, else the first that does.
                List<WindowsNetworks.Net> order = new ArrayList<>();
                if (def != null) order.add(def);
                if (kind == Kind.BEST) {
                    for (WindowsNetworks.Net n : s.networks) if (n != def) order.add(n);
                }
                for (WindowsNetworks.Net n : order) {
                    NetworkCapabilities nc = capabilitiesOf(s, n);
                    if (kind == Kind.DEFAULT || matches(nc)) {
                        targets.put(n.netId(), n);
                        caps.put(n.netId(), nc);
                        break;
                    }
                }
            }
            List<Integer> lost = new ArrayList<>();
            for (Integer netId : current.keySet()) {
                if (!targets.containsKey(netId)) lost.add(netId);
            }
            for (Map.Entry<Integer, WindowsNetworks.Net> e : targets.entrySet()) {
                if (!active) return;
                int netId = e.getKey();
                Network network = new Network(netId);
                NetworkCapabilities nc = caps.get(netId);
                LinkProperties lp = linkPropertiesOf(e.getValue());
                NetworkCapabilities was = current.get(netId);
                if (was == null) {
                    current.put(netId, nc);
                    currentLp.put(netId, lp);
                    everAvailable = true;
                    callback.onAvailable(network, new NetworkCapabilities(nc),
                            new LinkProperties(lp), false);
                } else {
                    if (!was.equals(nc)) {
                        current.put(netId, nc);
                        callback.onCapabilitiesChanged(network, new NetworkCapabilities(nc));
                    }
                    if (!lp.equals(currentLp.get(netId))) {
                        currentLp.put(netId, lp);
                        callback.onLinkPropertiesChanged(network, new LinkProperties(lp));
                    }
                }
            }
            for (Integer netId : lost) {
                if (!active) return;
                current.remove(netId);
                currentLp.remove(netId);
                callback.onLost(new Network(netId));
            }
        }
    }

    /** As ConnectivityManager.NetworkCallback. */
    public static class NetworkCallback {
        public static final int FLAG_NONE = 0;
        public static final int FLAG_INCLUDE_LOCATION_INFO = 1;

        public NetworkCallback() {
        }

        public NetworkCallback(int flags) {
        }

        public void onPreCheck(Network network) {
        }

        /**
         * A network is available, with what the module tells of it in this order.
         * @hide
         */
        public void onAvailable(Network network, NetworkCapabilities networkCapabilities,
                LinkProperties linkProperties, boolean blocked) {
            onAvailable(network);
            onCapabilitiesChanged(network, networkCapabilities);
            onLinkPropertiesChanged(network, linkProperties);
            onBlockedStatusChanged(network, blocked);
        }

        public void onAvailable(Network network) {
        }

        public void onBlockedStatusChanged(Network network, boolean blocked) {
        }

        public void onLosing(Network network, int maxMsToLive) {
        }

        public void onLost(Network network) {
        }

        public void onUnavailable() {
        }

        public void onCapabilitiesChanged(Network network,
                NetworkCapabilities networkCapabilities) {
        }

        public void onLinkPropertiesChanged(Network network, LinkProperties linkProperties) {
        }

        /** @hide */
        public void onNetworkSuspended(Network network) {
        }

        /** @hide */
        public void onNetworkResumed(Network network) {
        }
    }

    /** As ConnectivityManager.OnNetworkActiveListener. */
    public interface OnNetworkActiveListener {
        void onNetworkActive();
    }
}
