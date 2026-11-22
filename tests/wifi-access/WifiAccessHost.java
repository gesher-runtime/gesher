/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.net;

import android.app.ActivityThread;
import android.app.Application;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.net.ConnectivityManager;
import android.net.DhcpInfo;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkInfo;
import android.net.NetworkRequest;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiManager;
import android.net.wifi.WifiSsid;
import android.net.wifi.SupplicantState;
import android.net.wifi.ScanResult;
import android.net.wifi.WifiConfiguration;
import android.os.Handler;
import android.os.IBinder;
import android.os.IServiceManager;
import android.os.Message;
import android.os.MessageQueue;
import android.os.RemoteException;
import android.os.ServiceManager;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.atomic.AtomicLong;
import sun.misc.Unsafe;

/** Host-only adversarial callers of the production Java permission and network boundary. */
public final class WifiAccessHost {
    private static final String ACCESS = "android.permission.ACCESS_WIFI_STATE";
    private static final String CHANGE = "android.permission.CHANGE_WIFI_STATE";
    private static final String FINE = "android.permission.ACCESS_FINE_LOCATION";
    private static final String COARSE = "android.permission.ACCESS_COARSE_LOCATION";
    private static final String NETWORKS = "internet\t1\n"
            + "net\t12\twifi\t1500\t1000000\t1000000\t0\t0\t0\t1\t1\n"
            + "net\t13\twifi\t1500\t2000000\t2000000\t0\t0\t0\t1\t0\n";
    private static int checks;
    private static final Unsafe UNSAFE;
    static {
        try { Field f = Unsafe.class.getDeclaredField("theUnsafe"); f.setAccessible(true); UNSAFE = (Unsafe)f.get(null); }
        catch (Exception e) { throw new ExceptionInInitializerError(e); }
    }
    private interface Action { void run() throws Exception; }
    private static void check(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
        checks++;
    }
    private static void rejects(Class<? extends Throwable> expected, Action action, String reason) throws Exception {
        try { action.run(); }
        catch (Throwable e) { check(expected.isInstance(e), reason + ": " + e); return; }
        throw new AssertionError(reason + ": accepted");
    }
    private static Field field(Class<?> owner, String name) throws Exception {
        Field f = owner.getDeclaredField(name); f.setAccessible(true); return f;
    }
    private static final class Caller extends ContextWrapper {
        final Set<String> permissions = new HashSet<>();
        final ApplicationInfo info = new ApplicationInfo();
        int serviceLookups;
        boolean failPermission;
        int broadcasts;
        Caller() {
            super(null);
            info.uid = 10000;
            info.targetSdkVersion = 36;
            info.packageName = "com.gesher.wifiaccess";
        }
        @Override public int checkSelfPermission(String permission) {
            if (failPermission) throw new SecurityException("controlled permission lookup failure");
            return permissions.contains(permission) ? PackageManager.PERMISSION_GRANTED : PackageManager.PERMISSION_DENIED;
        }
        @Override public ApplicationInfo getApplicationInfo() { return info; }
        @Override public PackageManager getPackageManager() { return null; }
        @Override public Context getApplicationContext() { return this; }
        @Override public String getPackageName() { return info.packageName; }
        @Override public String getOpPackageName() { return info.packageName; }
        @Override public String getAttributionTag() { return "wifi-host"; }
        @Override public String getSystemServiceName(Class<?> type) { serviceLookups++; return null; }
        @Override public Object getSystemService(String name) { serviceLookups++; return null; }
        @Override public void sendBroadcast(Intent intent) { broadcasts++; }
    }

    private static Caller allowed() {
        Caller caller = new Caller();
        caller.permissions.addAll(List.of(ACCESS, CHANGE, FINE, COARSE));
        return caller;
    }

    private static WifiManager wifi(Context context) throws Exception {
        Constructor<WifiManager> ctor = WifiManager.class.getDeclaredConstructor(Context.class);
        ctor.setAccessible(true);
        return ctor.newInstance(context);
    }

    private static void hidden(WifiInfo info, String reason) {
        check(info != null, reason + " retains transport information");
        check(WifiManager.UNKNOWN_SSID.equals(info.getSSID()), reason + " SSID redacted");
        check(info.getBSSID() == null || WifiInfo.DEFAULT_MAC_ADDRESS.equals(info.getBSSID()), reason + " BSSID redacted");
        check(WifiInfo.DEFAULT_MAC_ADDRESS.equals(info.getMacAddress()), reason + " local MAC redacted");
        check(info.getNetworkId() == -1, reason + " saved network identifier redacted");
    }

    private static void helper() throws Exception {
        Caller caller = allowed();
        check(WifiAccess.context(caller) == caller, "explicit context identity retained");
        rejects(SecurityException.class, () -> WifiAccess.enforceAccess(null), "missing context cannot read Wi-Fi");
        rejects(SecurityException.class, () -> WifiAccess.enforceChange(null), "missing context cannot change Wi-Fi");
        WifiAccess.enforceAccess(caller);
        WifiAccess.enforceChange(caller);
        caller.permissions.remove(ACCESS);
        rejects(SecurityException.class, () -> WifiAccess.enforceAccess(caller), "missing ACCESS manifest grant rejected");
        caller.permissions.add(ACCESS);
        caller.permissions.remove(CHANGE);
        rejects(SecurityException.class, () -> WifiAccess.enforceChange(caller), "missing CHANGE manifest grant rejected");
        caller.permissions.add(CHANGE);
        caller.failPermission = true;
        rejects(SecurityException.class, () -> WifiAccess.enforceAccess(caller), "failed permission lookup is not a grant");
        caller.failPermission = false;
        Field current = field(ActivityThread.class, "sCurrentActivityThread");
        Object saved = current.get(null);
        try {
            current.set(null, null);
            check(WifiAccess.context(null) == null, "no process application has no fallback identity");
            WifiManager absent = wifi(null);
            rejects(SecurityException.class, absent::getWifiState, "context-free construction cannot bypass access");
            Application application = new Application();
            field(ContextWrapper.class, "mBase").set(application, caller);
            ActivityThread thread = (ActivityThread) UNSAFE.allocateInstance(ActivityThread.class);
            field(ActivityThread.class, "mInitialApplication").set(thread, application);
            current.set(null, thread);
            check(WifiAccess.context(null) == application, "fallback uses actual current process application");
        } finally { current.set(null, saved); }
    }

    private static void wifiNoAccess() throws Exception {
        WifiManager denied = wifi(new Caller());
        rejects(SecurityException.class, denied::getWifiState, "state requires ACCESS_WIFI_STATE");
        rejects(SecurityException.class, denied::isWifiEnabled, "enabled query requires ACCESS_WIFI_STATE");
        rejects(SecurityException.class, denied::getConnectionInfo, "connection requires ACCESS_WIFI_STATE");
        rejects(SecurityException.class, denied::getDhcpInfo, "DHCP requires ACCESS_WIFI_STATE");
        rejects(SecurityException.class, denied::getScanResults, "scan results require ACCESS_WIFI_STATE");
        rejects(SecurityException.class, denied::getConfiguredNetworks, "configured networks require ACCESS_WIFI_STATE");
        rejects(SecurityException.class, denied::startScan, "scan request requires CHANGE_WIFI_STATE");
        rejects(SecurityException.class, () -> denied.setWifiEnabled(true), "radio mutation requires CHANGE_WIFI_STATE");
        rejects(SecurityException.class, () -> denied.enableNetwork(7, true), "enable requires CHANGE_WIFI_STATE");
        rejects(SecurityException.class, () -> denied.disableNetwork(7), "disable requires CHANGE_WIFI_STATE");
        rejects(SecurityException.class, denied::disconnect, "disconnect requires CHANGE_WIFI_STATE");
        rejects(SecurityException.class, denied::reconnect, "reconnect requires CHANGE_WIFI_STATE");
        rejects(SecurityException.class, () -> denied.addNetwork(new WifiConfiguration()), "add network requires CHANGE_WIFI_STATE");
        rejects(SecurityException.class, () -> denied.removeNetwork(7), "remove network requires CHANGE_WIFI_STATE");
        rejects(SecurityException.class, denied::saveConfiguration, "save requires CHANGE_WIFI_STATE");
    }

    private static void wifiQueries() throws Exception {
        wifiNoAccess();
        Caller caller = allowed();
        WifiManager manager = wifi(caller);
        WifiServiceBoundary service = new WifiServiceBoundary();
        Map<String, IBinder> cache = serviceCache();
        Object previousManager = field(ServiceManager.class, "sServiceManager").get(null);
        IBinder previousWifi = cache.put(IWindowsWifi.NAME, service);
        try {
            check(manager.getWifiState() == WifiManager.WIFI_STATE_ENABLED, "state query forwards service state");
            check(caller.info.packageName.equals(service.name), "manager forwards its owning package");
            check(manager.getConnectionInfo() == service.info, "manager returns service-owned disclosure result");
            check("wifi-host".equals(service.tag), "connection request retains attribution");
            check(manager.getDhcpInfo() == service.dhcp, "DHCP response crosses client boundary unchanged");
            check(manager.getConfiguredNetworks().isEmpty(), "unsupported saved configurations remain empty");
            check(manager.getScanResults().size() == 1, "authorized scan read reaches service");
            check("wifi-host".equals(service.tag), "scan read retains attribution");
            check(manager.startScan() && service.starts == 1, "authorized scan reaches service");
            check(caller.broadcasts == 0, "client does not attempt a protected scan-completion broadcast");
            check(!manager.setWifiEnabled(true) && !manager.enableNetwork(7, true)
                    && !manager.disableNetwork(7) && !manager.disconnect() && !manager.reconnect()
                    && manager.addNetwork(new WifiConfiguration()) == -1 && !manager.removeNetwork(7)
                    && !manager.saveConfiguration(), "unsupported radio mutations do not report success");
            caller.serviceLookups = 0;
            caller.permissions.remove(FINE);
            hidden(manager.getConnectionInfo(), "client preserves service redaction after permission change");
            // These isolated local-context states must not preempt the authoritative
            // Binder service's caller identity, location policy or AppOps accounting.
            caller.permissions.add("android.permission.NETWORK_SETTINGS");
            check(manager.getScanResults().size() == 1 && service.reads == 2,
                    "scan read delegates location/AppOps policy to service");
            check(manager.startScan() && service.starts == 2,
                    "scan admission delegates location/AppOps policy to service");
            check(caller.serviceLookups == 0, "facade does not duplicate server location/AppOps queries");
            service.accept = false;
            check(!manager.startScan(), "service refusal stays false");
            caller.permissions.remove(CHANGE);
            rejects(SecurityException.class, manager::startScan, "runtime change grant loss denies facade scan");
            caller.permissions.add(CHANGE);
            caller.permissions.remove(ACCESS);
            rejects(SecurityException.class, manager::getScanResults, "access grant loss denies facade read");
            caller.permissions.add(ACCESS);
            service.remoteFailure = true;
            rejects(RuntimeException.class, manager::getWifiState, "remote service failure is surfaced");
            service.remoteFailure = false;
            service.securityFailure = true;
            rejects(SecurityException.class, manager::getConnectionInfo, "server caller denial is preserved");
            service.securityFailure = false;
            cache.remove(IWindowsWifi.NAME);
            field(ServiceManager.class, "sServiceManager").set(null, Proxy.newProxyInstance(
                    WifiAccessHost.class.getClassLoader(), new Class<?>[] {IServiceManager.class},
                    (proxy, method, arguments) -> method.getName().equals("getService2")
                            ? android.os.Service.serviceWithMetadata(new android.os.ServiceWithMetadata()) : null));
            check(manager.getWifiState() == WifiManager.WIFI_STATE_UNKNOWN, "missing service reports unknown, not disabled");
            check(!manager.isWifiEnabled(), "missing service is not enabled");
            check(manager.getConnectionInfo() != null && manager.getDhcpInfo() != null,
                    "missing service yields Android empty connection objects");
            check(manager.getConfiguredNetworks().isEmpty() && manager.getScanResults().isEmpty()
                    && !manager.startScan(), "missing service cannot invent cached scans or scan success");
        } finally {
            field(ServiceManager.class, "sServiceManager").set(null, previousManager);
            if (previousWifi == null) cache.remove(IWindowsWifi.NAME);
            else cache.put(IWindowsWifi.NAME, previousWifi);
        }
    }

    @SuppressWarnings("unchecked")
    private static Map<String, IBinder> serviceCache() throws Exception {
        return (Map<String, IBinder>) field(ServiceManager.class, "sCache").get(null);
    }

    /** Only the local Binder boundary is controlled; server policy has its separate harness. */
    private static final class WifiServiceBoundary extends IWindowsWifi.Stub {
        final WifiInfo info = new WifiInfo().makeCopy(7L);
        final DhcpInfo dhcp = new DhcpInfo();
        final List<NetworkRead> networkReads = new ArrayList<>();
        String name;
        String tag;
        int reads;
        int starts;
        boolean accept = true;
        boolean remoteFailure;
        boolean securityFailure;
        boolean disclose = true;
        boolean nullNetwork;
        private record NetworkRead(String name, String tag, int index, boolean includeLocation) { }
        private static WifiInfo observed(int index) {
            WifiInfo observed = new WifiInfo();
            observed.setSSID(WifiSsid.fromUtf8Text(index == 12 ? "NetworkA" : "NetworkB"));
            observed.setBSSID(index == 12 ? "10:20:30:40:50:60" : "10:20:30:40:50:61");
            observed.setSupplicantState(SupplicantState.COMPLETED);
            observed.setRssi(-42);
            return observed;
        }
        private void record(String name, String tag) throws RemoteException {
            this.name = name; this.tag = tag;
            if (remoteFailure) throw new RemoteException("controlled Wi-Fi service failure");
            if (securityFailure) throw new SecurityException("controlled server denial");
        }
        @Override public int getWifiState(String name) throws RemoteException {
            record(name, null); return WifiManager.WIFI_STATE_ENABLED;
        }
        @Override public WifiInfo getConnectionInfo(String name, String tag) throws RemoteException {
            record(name, tag); return info;
        }
        @Override public WifiInfo getConnectionInfoForNetwork(String name, String tag,
                int index, boolean includeLocation) throws RemoteException {
            networkReads.add(new NetworkRead(name, tag, index, includeLocation));
            record(name, tag);
            if (nullNetwork) return null;
            if (index != 12 && index != 13) return new WifiInfo();
            // Canned authoritative replies: no client permission or AppOps decision
            // is implemented here. WifiService's independent harness tests that policy.
            boolean visible = disclose && includeLocation && "com.gesher.wifiaccess".equals(name);
            return observed(index).makeCopy(visible ? 6L : 7L);
        }
        @Override public DhcpInfo getDhcpInfo(String name) throws RemoteException {
            record(name, null); return dhcp;
        }
        @Override public List<WifiConfiguration> getConfiguredNetworks(String name, String tag) throws RemoteException {
            record(name, tag); return List.of();
        }
        @Override public List<ScanResult> getScanResults(String name, String tag) throws RemoteException {
            record(name, tag); reads++; return List.of(new ScanResult());
        }
        @Override public boolean startScan(String name, String tag) throws RemoteException {
            record(name, tag); starts++; return accept;
        }
    }

    /** Controlled enqueue/dispatch using real Message and MessageQueue removal semantics. */
    private static final class Queue extends Handler {
        List<Message> messages;
        MessageQueue queue;
        private Queue() { super(); }
        static Queue create() throws Exception {
            Queue q = (Queue) UNSAFE.allocateInstance(Queue.class);
            q.messages = new ArrayList<>();
            q.queue = (MessageQueue) UNSAFE.allocateInstance(MessageQueue.class);
            field(MessageQueue.class, "sUseConcurrentInitialized").setBoolean(null, true);
            field(MessageQueue.class, "sUseConcurrent").setBoolean(null, false);
            field(MessageQueue.class, "mMessageCount").set(q.queue, new AtomicLong());
            field(Handler.class, "mQueue").set(q, q.queue);
            return q;
        }
        @Override public boolean sendMessageAtTime(Message message, long when) {
            try {
                message.setTarget(this);
                field(Message.class, "next").set(message, field(MessageQueue.class, "mMessages").get(queue));
                field(MessageQueue.class, "mMessages").set(queue, message);
                ((AtomicLong) field(MessageQueue.class, "mMessageCount").get(queue)).incrementAndGet();
                messages.add(message);
                return true;
            } catch (Exception e) { throw new AssertionError(e); }
        }
        void drain() {
            while (!messages.isEmpty()) {
                Runnable callback = messages.remove(0).getCallback();
                if (callback != null) { removeCallbacks(callback); callback.run(); }
            }
        }
    }

    private static final class Service extends IConnectivity.Stub {
        IConnectivity.IListener listener;
        @Override public String getSnapshot() { return NETWORKS; }
        @Override public void addListener(IConnectivity.IListener listener) { this.listener = listener; }
        @Override public void removeListener(IConnectivity.IListener listener) { if (this.listener == listener) this.listener = null; }
    }
    private static final class Callback extends ConnectivityManager.NetworkCallback {
        final Map<Integer, WifiInfo> latest = new LinkedHashMap<>();
        int changes;
        Callback(int flags) { super(flags); }
        @Override public void onCapabilitiesChanged(Network network, NetworkCapabilities capabilities) {
            latest.put(network.netId, (WifiInfo) capabilities.getTransportInfo());
            changes++;
        }
    }

    private static void connectivity() throws Exception {
        Service service = new Service();
        field(ConnectivityManager.class, "sService").set(null, service);
        WifiServiceBoundary wifiService = new WifiServiceBoundary();
        Map<String, IBinder> cache = serviceCache();
        IBinder previousWifi = cache.put(IWindowsWifi.NAME, wifiService);
        try {
            Caller caller = allowed();
            ConnectivityManager manager = new ConnectivityManager(caller);
            hidden((WifiInfo) manager.getNetworkCapabilities(new Network(112)).getTransportInfo(), "synchronous capability query");
            check(wifiService.networkReads.size() == 1 && wifiService.networkReads.get(0).index() == 12
                    && !wifiService.networkReads.get(0).includeLocation(),
                    "synchronous capability query forwards exact interface with location excluded");
            hidden((WifiInfo) ConnectivityManager.capabilitiesOf(WindowsNetworks.parse(NETWORKS),
                    WindowsNetworks.parse(NETWORKS).byNetId(113)).getTransportInfo(), "generic static capability helper");
            NetworkInfo broadcastInfo = ConnectivityManager.connectivityAction(WindowsNetworks.parse(NETWORKS))
                    .getParcelableExtra(ConnectivityManager.EXTRA_NETWORK_INFO, NetworkInfo.class);
            check(broadcastInfo != null && broadcastInfo.getExtraInfo() == null, "sticky broadcast cannot disclose SSID");
            check(wifiService.networkReads.size() == 1, "generic helper and sticky broadcast have no caller disclosure");
            check("\"NetworkB\"".equals(manager.getNetworkInfo(new Network(113)).getExtraInfo()),
                    "legacy query identifies requested adapter, not default Wi-Fi");
            check(wifiService.networkReads.get(1).index() == 13 && wifiService.networkReads.get(1).includeLocation(),
                    "legacy per-network query requests service-controlled disclosure for exact interface");
            Queue queue = Queue.create();
            NetworkRequest request = new NetworkRequest.Builder().clearCapabilities().build();
            Callback def = new Callback(0);
            Callback opt = new Callback(ConnectivityManager.NetworkCallback.FLAG_INCLUDE_LOCATION_INFO);
            Callback denied = new Callback(ConnectivityManager.NetworkCallback.FLAG_INCLUDE_LOCATION_INFO);
            Callback fallback = new Callback(ConnectivityManager.NetworkCallback.FLAG_INCLUDE_LOCATION_INFO);
            Callback noContext = new Callback(ConnectivityManager.NetworkCallback.FLAG_INCLUDE_LOCATION_INFO);
            rejects(IllegalArgumentException.class, () -> new Callback(2), "unknown callback flags rejected");
            caller.serviceLookups = 0;
            wifiService.networkReads.clear();
            manager.registerNetworkCallback(request, def, queue);
            queue.drain();
            check(def.latest.size() == 2, "default callback receives both observed networks");
            hidden(def.latest.get(112), "default callback first adapter");
            hidden(def.latest.get(113), "default callback second adapter");
            check(wifiService.networkReads.size() == 2
                    && wifiService.networkReads.stream().noneMatch(WifiServiceBoundary.NetworkRead::includeLocation),
                    "default callback excludes location in every service request");
            check(caller.serviceLookups == 0, "callback client does not evaluate or note local location/AppOps");
            manager.registerNetworkCallback(request, opt, queue);
            Caller forged = allowed();
            forged.info.packageName = "com.gesher.denied";
            forged.permissions.add("android.permission.NETWORK_SETTINGS");
            new ConnectivityManager(forged).registerNetworkCallback(request, denied, queue);
            queue.drain();
            check("\"NetworkA\"".equals(opt.latest.get(112).getSSID()), "opt-in callback receives first adapter identity");
            check("\"NetworkB\"".equals(opt.latest.get(113).getSSID()), "opt-in callback receives matching second adapter identity");
            check(wifiService.networkReads.stream().anyMatch(read -> read.includeLocation() && read.index() == 13
                    && caller.info.packageName.equals(read.name()) && "wifi-host".equals(read.tag())),
                    "opt-in callback forwards index, package, attribution and flag");
            hidden(denied.latest.get(112), "forged client privileged permission cannot elevate service reply");
            Field currentApp = field(ActivityThread.class, "sCurrentActivityThread");
            Object savedApp = currentApp.get(null);
            try {
                currentApp.set(null, null);
                ConnectivityManager noContextManager = new ConnectivityManager(null);
                Application application = new Application();
                field(ContextWrapper.class, "mBase").set(application, caller);
                ActivityThread thread = (ActivityThread) UNSAFE.allocateInstance(ActivityThread.class);
                field(ActivityThread.class, "mInitialApplication").set(thread, application);
                currentApp.set(null, thread);
                ConnectivityManager fallbackManager = new ConnectivityManager(null);
                noContextManager.registerNetworkCallback(request, noContext, queue);
                fallbackManager.registerNetworkCallback(request, fallback, queue);
                currentApp.set(null, null);
                queue.drain();
                hidden(noContext.latest.get(112), "absent construction context remains fail-closed with later global application");
                check("\"NetworkA\"".equals(fallback.latest.get(112).getSSID()),
                        "fallback callback retains construction application after global context disappears");
            } finally { currentApp.set(null, savedApp); }
            int before = opt.changes;
            // The service's reply changes while the adapter snapshot remains identical.
            // The client must obtain the current reply for every delivery.
            wifiService.disclose = false;
            caller.permissions.add("android.permission.NETWORK_SETTINGS");
            service.listener.onSnapshot(NETWORKS);
            queue.drain();
            check(opt.changes > before, "same network snapshot updates redaction after authoritative permission revoke");
            hidden(opt.latest.get(112), "opt-in callback after authoritative revoke despite forged local grant");
            check(manager.getActiveNetworkInfo().getExtraInfo() == null
                    && manager.getNetworkInfo(ConnectivityManager.TYPE_WIFI).getExtraInfo() == null,
                    "legacy active and type queries preserve authoritative redaction");
            for (NetworkInfo legacy : manager.getAllNetworkInfo()) {
                check(legacy.getExtraInfo() == null, "legacy all-network query preserves each redacted SSID");
            }
            wifiService.disclose = true;
            service.listener.onSnapshot(NETWORKS);
            queue.drain();
            check("\"NetworkA\"".equals(opt.latest.get(112).getSSID()),
                    "callback recomputes permitted disclosure when server grant becomes available");
            service.listener.onSnapshot(NETWORKS);
            wifiService.disclose = false;
            queue.drain();
            hidden(opt.latest.get(112), "queued callback obtains current server policy at delivery");
            wifiService.disclose = true;
            wifiService.securityFailure = true;
            hidden((WifiInfo) manager.getNetworkCapabilities(new Network(112)).getTransportInfo(), "server identity rejection");
            service.listener.onSnapshot(NETWORKS);
            queue.drain();
            hidden(opt.latest.get(112), "callback service denial does not restore raw backend information");
            wifiService.securityFailure = false;
            wifiService.remoteFailure = true;
            hidden((WifiInfo) manager.getNetworkCapabilities(new Network(112)).getTransportInfo(), "optional transport service failure");
            wifiService.remoteFailure = false;
            wifiService.nullNetwork = true;
            hidden((WifiInfo) manager.getNetworkCapabilities(new Network(112)).getTransportInfo(), "optional transport service returned no detail");
            wifiService.nullNetwork = false;
            manager.unregisterNetworkCallback(def);
            manager.unregisterNetworkCallback(opt);
            manager.unregisterNetworkCallback(denied);
            manager.unregisterNetworkCallback(fallback);
            manager.unregisterNetworkCallback(noContext);
            check(service.listener == null, "last callback retires shared listener");
        } finally {
            field(ConnectivityManager.class, "sService").set(null, null);
            if (previousWifi == null) cache.remove(IWindowsWifi.NAME);
            else cache.put(IWindowsWifi.NAME, previousWifi);
        }
    }

    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        String selected = args.length > 1 ? args[1] : "all";
        if (selected.equals("all") || selected.equals("helper")) helper();
        if (selected.equals("all") || selected.equals("wifi")) wifiQueries();
        if (selected.equals("wifi-no-access")) wifiNoAccess();
        if (selected.equals("all") || selected.equals("connectivity")) connectivity();
        System.out.println("WifiAccessHost passed: case=" + selected + " checks=" + checks
                + "; production caller permissions/facade/callbacks; controlled Context, local Binder replies and Handler boundaries");
    }
}
