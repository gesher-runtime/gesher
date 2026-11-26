/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.net;

import android.content.pm.ParceledListSlice;
import android.net.DhcpInfo;
import android.net.wifi.BlockingOption;
import android.net.wifi.ScanResult;
import android.net.wifi.WifiConfiguration;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiNetworkSpecifier;
import android.net.wifi.WifiNetworkSuggestion;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Parcel;
import android.os.Parcelable;
import android.os.RemoteException;
import java.util.List;

/** Internal Wi-Fi transport. Authentication and disclosure policy belong to its service. */
public interface IWindowsWifi extends IInterface {
    String NAME = "gesher.wifi";
    String DESCRIPTOR = "com.gesher.net.IWindowsWifi";

    /** {@link #getCapabilities} groups: feature queries need ACCESS_WIFI_STATE, bands do not. */
    int CAPABILITIES_FEATURES = 0;
    int CAPABILITIES_BANDS = 1;
    /** Feature bits, from the adapters' supported infrastructure auth/cipher pairs. */
    long FEATURE_WPA3_SAE = 1, FEATURE_WPA3_SUITE_B = 1 << 1, FEATURE_OWE = 1 << 2,
            FEATURE_WPA_PERSONAL = 1 << 3, FEATURE_WEP = 1 << 4;
    /** Band bits (from PHY types or observed BSSs) and Wi-Fi standard bits (PHY types). */
    long BAND_24_GHZ = 1, BAND_5_GHZ = 1 << 1, BAND_6_GHZ = 1 << 2, BAND_60_GHZ = 1 << 3;
    int STANDARD_SHIFT = 16; // bit STANDARD_SHIFT + ScanResult.WIFI_STANDARD_*

    /** {@link #registerCallback} kinds. */
    int CALLBACK_SCAN_RESULTS = 1;
    int CALLBACK_WIFI_STATE = 2;
    int CALLBACK_SUGGESTION_APPROVAL = 3;
    int CALLBACK_SUGGESTION_CONNECTION = 4;
    int CALLBACK_LOCAL_ONLY_FAILURE = 5;
    /** Events of one {@link #requestLocalNetwork} request (the value is NETWORK_*). */
    int CALLBACK_NETWORK_REQUEST = 6;
    int NETWORK_AVAILABLE = 1, NETWORK_UNAVAILABLE = 2, NETWORK_LOST = 3;
    /**
     * NETWORK_AVAILABLE's only extra: the joined interface. The joined SSID/BSSID reach the
     * app solely through the location-checked WifiInfo; CALLBACK_LOCAL_ONLY_FAILURE carries
     * the failed request's specifier.
     */
    String EXTRA_INTERFACE = "interface", EXTRA_SPECIFIER = "specifier";

    int getWifiState(String packageName) throws RemoteException;
    WifiInfo getConnectionInfo(String packageName, String attributionTag) throws RemoteException;
    WifiInfo getConnectionInfoForNetwork(String packageName, String attributionTag,
            int interfaceIndex, boolean includeLocation) throws RemoteException;
    DhcpInfo getDhcpInfo(String packageName) throws RemoteException;
    List<WifiConfiguration> getConfiguredNetworks(String packageName, String attributionTag)
            throws RemoteException;
    List<ScanResult> getScanResults(String packageName, String attributionTag) throws RemoteException;
    boolean startScan(String packageName, String attributionTag) throws RemoteException;
    /** Feature or band/standard bits of the present adapters (see the constants above). */
    long getCapabilities(String packageName, int group) throws RemoteException;
    /** Registers a {@link Callback} binder for one {@code CALLBACK_*} kind. */
    void registerCallback(String packageName, String attributionTag, int kind, IBinder callback)
            throws RemoteException;
    void unregisterCallback(String packageName, int kind, IBinder callback) throws RemoteException;
    /** Per-frequency AP counts of the cached scan, as WifiManager.getChannelData. */
    List<Bundle> getChannelData(String packageName, String attributionTag) throws RemoteException;
    /** A ConnectivityManager request with a WifiNetworkSpecifier; events go to callback. */
    void requestLocalNetwork(String packageName, String attributionTag, WifiNetworkSpecifier specifier,
            IBinder callback) throws RemoteException;
    void releaseLocalNetwork(String packageName, IBinder callback) throws RemoteException;
    int addNetworkSuggestions(String packageName, String attributionTag, List<WifiNetworkSuggestion> suggestions)
            throws RemoteException;
    int removeNetworkSuggestions(String packageName, String attributionTag, List<WifiNetworkSuggestion> suggestions,
            int action) throws RemoteException;
    List<WifiNetworkSuggestion> getNetworkSuggestions(String packageName) throws RemoteException;
    void disallowCurrentSuggestedNetwork(String packageName, BlockingOption option) throws RemoteException;

    /** Client end of service events; the service only sends one-way transactions to it. */
    abstract class Callback extends Binder {
        static final String DESCRIPTOR = "com.gesher.net.IWindowsWifi.Callback";
        static final int EVENT = IBinder.FIRST_CALL_TRANSACTION;

        protected Callback() { attachInterface(null, DESCRIPTOR); }

        /** Called on a Binder thread; implementations hand off to the app's executor. */
        protected abstract void onEvent(int kind, int value, Bundle extras);

        @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                throws RemoteException {
            if (code != EVENT) return super.onTransact(code, data, reply, flags);
            data.enforceInterface(DESCRIPTOR);
            int kind = data.readInt(), value = data.readInt();
            Bundle extras = data.readTypedObject(Bundle.CREATOR);
            onEvent(kind, value, extras);
            return true;
        }

        /** Service side: deliver without waiting for the client (FLAG_ONEWAY). */
        public static boolean send(IBinder callback, int kind, int value, Bundle extras) {
            if (callback instanceof Callback local) {
                // Same-process client (a standalone app in the system process): a local
                // Binder call is synchronous anyway; onEvent only hands off to an executor.
                try { local.onEvent(kind, value, extras); return true; }
                catch (RuntimeException failed) { return false; }
            }
            Parcel data = Parcel.obtain();
            try {
                data.writeInterfaceToken(DESCRIPTOR);
                data.writeInt(kind); data.writeInt(value);
                data.writeTypedObject(extras, 0);
                return callback.transact(EVENT, data, null, IBinder.FLAG_ONEWAY);
            } catch (RemoteException | RuntimeException gone) {
                return false;
            } finally {
                data.recycle();
            }
        }
    }

    abstract class Stub extends Binder implements IWindowsWifi {
        private static final int STATE = IBinder.FIRST_CALL_TRANSACTION;
        private static final int CONNECTION = STATE + 1;
        private static final int DHCP = STATE + 2;
        private static final int CONFIGURED = STATE + 3;
        private static final int RESULTS = STATE + 4;
        private static final int SCAN = STATE + 5;
        private static final int NETWORK = STATE + 6;
        private static final int CAPABILITIES = STATE + 7;
        private static final int REGISTER = STATE + 8;
        private static final int UNREGISTER = STATE + 9;
        private static final int CHANNELS = STATE + 10;
        private static final int REQUEST = STATE + 11;
        private static final int RELEASE = STATE + 12;
        private static final int SUGGEST = STATE + 13;
        private static final int UNSUGGEST = STATE + 14;
        private static final int SUGGESTIONS = STATE + 15;
        private static final int DISALLOW = STATE + 16;

        protected Stub() { attachInterface(this, DESCRIPTOR); }

        public static IWindowsWifi asInterface(IBinder binder) {
            if (binder == null) return null;
            IInterface local = binder.queryLocalInterface(DESCRIPTOR);
            return local instanceof IWindowsWifi wifi ? wifi : new Proxy(binder);
        }

        @Override public IBinder asBinder() { return this; }

        @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                throws RemoteException {
            if (code < STATE || code > DISALLOW) return super.onTransact(code, data, reply, flags);
            data.enforceInterface(DESCRIPTOR);
            String packageName = data.readString();
            String attributionTag = data.readString();
            int interfaceIndex = code == NETWORK ? data.readInt() : 0;
            boolean includeLocation = code == NETWORK && data.readBoolean();
            int argument = code == CAPABILITIES || code == REGISTER || code == UNREGISTER ? data.readInt() : 0;
            WifiNetworkSpecifier specifier = code == REQUEST ? data.readTypedObject(WifiNetworkSpecifier.CREATOR) : null;
            List<WifiNetworkSuggestion> suggestions = null;
            if (code == SUGGEST || code == UNSUGGEST) {
                ParceledListSlice<WifiNetworkSuggestion> slice = data.readTypedObject(ParceledListSlice.CREATOR);
                suggestions = slice == null ? null : slice.getList();
                if (code == UNSUGGEST) argument = data.readInt();
            }
            BlockingOption option = code == DISALLOW ? data.readTypedObject(BlockingOption.CREATOR) : null;
            IBinder callback = code == REGISTER || code == UNREGISTER || code == REQUEST || code == RELEASE
                    ? data.readStrongBinder() : null;
            data.enforceNoDataAvail();
            switch (code) {
                case STATE: {
                    int result = getWifiState(packageName);
                    reply.writeNoException(); reply.writeInt(result); break;
                }
                case CONNECTION: {
                    WifiInfo result = getConnectionInfo(packageName, attributionTag);
                    reply.writeNoException(); reply.writeTypedObject(result, Parcelable.PARCELABLE_WRITE_RETURN_VALUE);
                    break;
                }
                case DHCP: {
                    DhcpInfo result = getDhcpInfo(packageName);
                    reply.writeNoException(); reply.writeTypedObject(result, Parcelable.PARCELABLE_WRITE_RETURN_VALUE);
                    break;
                }
                case NETWORK: {
                    WifiInfo result = getConnectionInfoForNetwork(packageName, attributionTag,
                            interfaceIndex, includeLocation);
                    reply.writeNoException(); reply.writeTypedObject(result, Parcelable.PARCELABLE_WRITE_RETURN_VALUE);
                    break;
                }
                case CONFIGURED: {
                    List<WifiConfiguration> result = getConfiguredNetworks(packageName, attributionTag);
                    reply.writeNoException(); writeList(reply, result); break;
                }
                case RESULTS: {
                    List<ScanResult> result = getScanResults(packageName, attributionTag);
                    reply.writeNoException(); writeList(reply, result); break;
                }
                case SCAN: {
                    boolean result = startScan(packageName, attributionTag);
                    reply.writeNoException(); reply.writeBoolean(result); break;
                }
                case CAPABILITIES: {
                    long result = getCapabilities(packageName, argument);
                    reply.writeNoException(); reply.writeLong(result); break;
                }
                case REGISTER: {
                    if (callback == null) throw new IllegalArgumentException("callback cannot be null");
                    registerCallback(packageName, attributionTag, argument, callback);
                    reply.writeNoException(); break;
                }
                case UNREGISTER: {
                    if (callback == null) throw new IllegalArgumentException("callback cannot be null");
                    unregisterCallback(packageName, argument, callback);
                    reply.writeNoException(); break;
                }
                case CHANNELS: {
                    List<Bundle> result = getChannelData(packageName, attributionTag);
                    reply.writeNoException(); writeList(reply, result); break;
                }
                case REQUEST: {
                    if (callback == null || specifier == null) throw new IllegalArgumentException("Invalid request");
                    requestLocalNetwork(packageName, attributionTag, specifier, callback);
                    reply.writeNoException(); break;
                }
                case RELEASE: {
                    if (callback == null) throw new IllegalArgumentException("callback cannot be null");
                    releaseLocalNetwork(packageName, callback);
                    reply.writeNoException(); break;
                }
                case SUGGEST: {
                    int result = addNetworkSuggestions(packageName, attributionTag, suggestions);
                    reply.writeNoException(); reply.writeInt(result); break;
                }
                case UNSUGGEST: {
                    int result = removeNetworkSuggestions(packageName, attributionTag, suggestions, argument);
                    reply.writeNoException(); reply.writeInt(result); break;
                }
                case SUGGESTIONS: {
                    List<WifiNetworkSuggestion> result = getNetworkSuggestions(packageName);
                    reply.writeNoException(); writeList(reply, result); break;
                }
                case DISALLOW: {
                    if (option == null) throw new IllegalArgumentException("blockingOption cannot be null");
                    disallowCurrentSuggestedNetwork(packageName, option);
                    reply.writeNoException(); break;
                }
                default: throw new AssertionError(code);
            }
            return true;
        }

        private static <T extends Parcelable> void writeList(Parcel reply, List<T> result) {
            // A dense WLAN environment must not overflow one Binder transaction or
            // silently truncate observations. Use Android's ordinary list transport.
            reply.writeTypedObject(result == null ? null : new ParceledListSlice<>(result),
                    Parcelable.PARCELABLE_WRITE_RETURN_VALUE);
        }

        private static final class Proxy implements IWindowsWifi {
            private final IBinder mRemote;
            Proxy(IBinder remote) { mRemote = remote; }
            @Override public IBinder asBinder() { return mRemote; }

            private interface Read<T> { T read(Parcel reply); }
            private interface Write { void write(Parcel data); }
            private <T> T call(int code, String name, String tag, Read<T> read)
                    throws RemoteException {
                return call(code, name, tag, null, read);
            }
            private <T> T call(int code, String name, String tag, Write extra, Read<T> read)
                    throws RemoteException {
                Parcel data = Parcel.obtain(), reply = Parcel.obtain();
                try {
                    data.writeInterfaceToken(DESCRIPTOR);
                    data.writeString(name); data.writeString(tag);
                    if (extra != null) extra.write(data);
                    if (!mRemote.transact(code, data, reply, 0)) {
                        throw new RemoteException("Wi-Fi service does not implement transaction " + code);
                    }
                    reply.readException();
                    return read.read(reply);
                } finally {
                    reply.recycle(); data.recycle();
                }
            }
            @Override public int getWifiState(String name) throws RemoteException {
                return call(STATE, name, null, Parcel::readInt);
            }
            @Override public WifiInfo getConnectionInfo(String name, String tag) throws RemoteException {
                return call(CONNECTION, name, tag, reply -> reply.readTypedObject(WifiInfo.CREATOR));
            }
            @Override public WifiInfo getConnectionInfoForNetwork(String name, String tag,
                    int index, boolean includeLocation) throws RemoteException {
                return call(NETWORK, name, tag, data -> { data.writeInt(index); data.writeBoolean(includeLocation); },
                        reply -> reply.readTypedObject(WifiInfo.CREATOR));
            }
            @Override public DhcpInfo getDhcpInfo(String name) throws RemoteException {
                return call(DHCP, name, null, reply -> reply.readTypedObject(DhcpInfo.CREATOR));
            }
            @Override public List<WifiConfiguration> getConfiguredNetworks(String name, String tag)
                    throws RemoteException {
                return call(CONFIGURED, name, tag, Proxy::readList);
            }
            @Override public List<ScanResult> getScanResults(String name, String tag) throws RemoteException {
                return call(RESULTS, name, tag, Proxy::readList);
            }
            @Override public boolean startScan(String name, String tag) throws RemoteException {
                return call(SCAN, name, tag, Parcel::readBoolean);
            }
            @Override public long getCapabilities(String name, int group) throws RemoteException {
                return call(CAPABILITIES, name, null, data -> data.writeInt(group), Parcel::readLong);
            }
            @Override public void registerCallback(String name, String tag, int kind, IBinder callback)
                    throws RemoteException {
                call(REGISTER, name, tag, data -> { data.writeInt(kind); data.writeStrongBinder(callback); },
                        reply -> null);
            }
            @Override public void unregisterCallback(String name, int kind, IBinder callback)
                    throws RemoteException {
                call(UNREGISTER, name, null, data -> { data.writeInt(kind); data.writeStrongBinder(callback); },
                        reply -> null);
            }
            @Override public List<Bundle> getChannelData(String name, String tag) throws RemoteException {
                return call(CHANNELS, name, tag, Proxy::readList);
            }
            @Override public void requestLocalNetwork(String name, String tag, WifiNetworkSpecifier specifier,
                    IBinder callback) throws RemoteException {
                call(REQUEST, name, tag, data -> { data.writeTypedObject(specifier, 0); data.writeStrongBinder(callback); },
                        reply -> null);
            }
            @Override public void releaseLocalNetwork(String name, IBinder callback) throws RemoteException {
                call(RELEASE, name, null, data -> data.writeStrongBinder(callback), reply -> null);
            }
            @Override public int addNetworkSuggestions(String name, String tag, List<WifiNetworkSuggestion> list)
                    throws RemoteException {
                return call(SUGGEST, name, tag, data -> data.writeTypedObject(
                        list == null ? null : new ParceledListSlice<>(list), 0), Parcel::readInt);
            }
            @Override public int removeNetworkSuggestions(String name, String tag, List<WifiNetworkSuggestion> list,
                    int action) throws RemoteException {
                return call(UNSUGGEST, name, tag, data -> {
                    data.writeTypedObject(list == null ? null : new ParceledListSlice<>(list), 0);
                    data.writeInt(action);
                }, Parcel::readInt);
            }
            @Override public List<WifiNetworkSuggestion> getNetworkSuggestions(String name) throws RemoteException {
                return call(SUGGESTIONS, name, null, Proxy::readList);
            }
            @Override public void disallowCurrentSuggestedNetwork(String name, BlockingOption option)
                    throws RemoteException {
                call(DISALLOW, name, null, data -> data.writeTypedObject(option, 0), reply -> null);
            }
            @SuppressWarnings("unchecked") private static <T extends Parcelable> List<T> readList(Parcel reply) {
                ParceledListSlice<T> result = reply.readTypedObject(ParceledListSlice.CREATOR);
                return result == null ? null : result.getList();
            }
        }
    }
}
