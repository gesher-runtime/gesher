/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.net;

import android.content.Context;
import android.os.Handler;

/**
 * A placeholder for the Connectivity module's ConnectivityManager (framework.jar compiles
 * against the module's API stubs and does not contain it), registered as
 * {@code connectivity} by {@link ConnectivityFrameworkInitializer}: real apps ask it whether
 * they are online before they fetch anything (docs/e2e.md). It answers like a device with
 * no network: no active network, no networks at all, callbacks registered and never called.
 * The app's own sockets are not affected (they use Windows' network directly).
 *
 * <p>The public API apps call, with the module's signatures; when framework.jar (or a
 * module jar ahead of gesher-app.jar on the boot class path) brings the real class, it wins.
 */
public class ConnectivityManager {
    public static final String CONNECTIVITY_ACTION = "android.net.conn.CONNECTIVITY_CHANGE";
    public static final String EXTRA_NETWORK_INFO = "networkInfo";
    public static final String EXTRA_NETWORK_TYPE = "networkType";
    public static final String EXTRA_NO_CONNECTIVITY = "noConnectivity";
    public static final String EXTRA_IS_FAILOVER = "isFailover";
    public static final String EXTRA_REASON = "reason";
    public static final String EXTRA_EXTRA_INFO = "extraInfo";
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
    public static final String ACTION_RESTRICT_BACKGROUND_CHANGED =
            "android.net.conn.RESTRICT_BACKGROUND_CHANGED";

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

    public NetworkInfo getActiveNetworkInfo() {
        return null;
    }

    public Network getActiveNetwork() {
        return null;
    }

    public Network[] getAllNetworks() {
        return new Network[0];
    }

    public NetworkInfo[] getAllNetworkInfo() {
        return new NetworkInfo[0];
    }

    public NetworkInfo getNetworkInfo(int networkType) {
        return null;
    }

    public NetworkInfo getNetworkInfo(Network network) {
        return null;
    }

    public NetworkCapabilities getNetworkCapabilities(Network network) {
        return null;
    }

    public LinkProperties getLinkProperties(Network network) {
        return null;
    }

    public int getNetworkPreference() {
        return TYPE_NONE;
    }

    /** No network: metered, as ConnectivityService answers without one. */
    public boolean isActiveNetworkMetered() {
        return true;
    }

    public int getRestrictBackgroundStatus() {
        return RESTRICT_BACKGROUND_STATUS_DISABLED;
    }

    @Deprecated
    public boolean getBackgroundDataSetting() {
        return true;
    }

    public boolean isDefaultNetworkActive() {
        return false;
    }

    public void addDefaultNetworkActiveListener(OnNetworkActiveListener l) {
    }

    public void removeDefaultNetworkActiveListener(OnNetworkActiveListener l) {
    }

    public void registerNetworkCallback(NetworkRequest request,
            NetworkCallback networkCallback) {
    }

    public void registerNetworkCallback(NetworkRequest request,
            NetworkCallback networkCallback, Handler handler) {
    }

    public void registerDefaultNetworkCallback(NetworkCallback networkCallback) {
    }

    public void registerDefaultNetworkCallback(NetworkCallback networkCallback,
            Handler handler) {
    }

    public void registerBestMatchingNetworkCallback(NetworkRequest request,
            NetworkCallback networkCallback, Handler handler) {
    }

    public void requestNetwork(NetworkRequest request,
            NetworkCallback networkCallback) {
    }

    public void requestNetwork(NetworkRequest request,
            NetworkCallback networkCallback, Handler handler) {
    }

    /** No network will come: onUnavailable once the timeout has passed, as on a device. */
    public void requestNetwork(NetworkRequest request,
            final NetworkCallback networkCallback, int timeoutMs) {
        requestNetwork(request, networkCallback, new Handler(android.os.Looper.getMainLooper()),
                timeoutMs);
    }

    public void requestNetwork(NetworkRequest request,
            final NetworkCallback networkCallback, Handler handler,
            int timeoutMs) {
        handler.postDelayed(networkCallback::onUnavailable, Math.max(0, timeoutMs));
    }

    public void unregisterNetworkCallback(NetworkCallback networkCallback) {
    }

    public void reportNetworkConnectivity(Network network, boolean hasConnectivity) {
    }

    @Deprecated
    public void reportBadNetwork(Network network) {
    }

    public boolean bindProcessToNetwork(Network network) {
        sProcessNetwork = network;
        return network == null;
    }

    public Network getBoundNetworkForProcess() {
        return sProcessNetwork;
    }

    @Deprecated
    public static boolean setProcessDefaultNetwork(Network network) {
        sProcessNetwork = network;
        return network == null;
    }

    @Deprecated
    public static Network getProcessDefaultNetwork() {
        return sProcessNetwork;
    }

    /** As ConnectivityManager.NetworkCallback: called back for no network. */
    public static class NetworkCallback {
        public static final int FLAG_NONE = 0;
        public static final int FLAG_INCLUDE_LOCATION_INFO = 1;

        public NetworkCallback() {
        }

        public NetworkCallback(int flags) {
        }

        public void onPreCheck(Network network) {
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

        public void onLinkPropertiesChanged(Network network,
                LinkProperties linkProperties) {
        }
    }

    /** As ConnectivityManager.OnNetworkActiveListener. */
    public interface OnNetworkActiveListener {
        void onNetworkActive();
    }
}
