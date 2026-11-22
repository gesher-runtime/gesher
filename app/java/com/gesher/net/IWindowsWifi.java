/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.net;

import android.content.pm.ParceledListSlice;
import android.net.DhcpInfo;
import android.net.wifi.ScanResult;
import android.net.wifi.WifiConfiguration;
import android.net.wifi.WifiInfo;
import android.os.Binder;
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

    int getWifiState(String packageName) throws RemoteException;
    WifiInfo getConnectionInfo(String packageName, String attributionTag) throws RemoteException;
    WifiInfo getConnectionInfoForNetwork(String packageName, String attributionTag,
            int interfaceIndex, boolean includeLocation) throws RemoteException;
    DhcpInfo getDhcpInfo(String packageName) throws RemoteException;
    List<WifiConfiguration> getConfiguredNetworks(String packageName, String attributionTag)
            throws RemoteException;
    List<ScanResult> getScanResults(String packageName, String attributionTag) throws RemoteException;
    boolean startScan(String packageName, String attributionTag) throws RemoteException;

    abstract class Stub extends Binder implements IWindowsWifi {
        private static final int STATE = IBinder.FIRST_CALL_TRANSACTION;
        private static final int CONNECTION = STATE + 1;
        private static final int DHCP = STATE + 2;
        private static final int CONFIGURED = STATE + 3;
        private static final int RESULTS = STATE + 4;
        private static final int SCAN = STATE + 5;
        private static final int NETWORK = STATE + 6;

        protected Stub() { attachInterface(this, DESCRIPTOR); }

        public static IWindowsWifi asInterface(IBinder binder) {
            if (binder == null) return null;
            IInterface local = binder.queryLocalInterface(DESCRIPTOR);
            return local instanceof IWindowsWifi wifi ? wifi : new Proxy(binder);
        }

        @Override public IBinder asBinder() { return this; }

        @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                throws RemoteException {
            if (code < STATE || code > NETWORK) return super.onTransact(code, data, reply, flags);
            data.enforceInterface(DESCRIPTOR);
            String packageName = data.readString();
            String attributionTag = data.readString();
            int interfaceIndex = code == NETWORK ? data.readInt() : 0;
            boolean includeLocation = code == NETWORK && data.readBoolean();
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
            private <T> T call(int code, String name, String tag, Read<T> read)
                    throws RemoteException {
                return call(code, name, tag, 0, false, read);
            }
            private <T> T call(int code, String name, String tag, int index, boolean includeLocation,
                    Read<T> read) throws RemoteException {
                Parcel data = Parcel.obtain(), reply = Parcel.obtain();
                try {
                    data.writeInterfaceToken(DESCRIPTOR);
                    data.writeString(name); data.writeString(tag);
                    if (code == NETWORK) { data.writeInt(index); data.writeBoolean(includeLocation); }
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
                return call(NETWORK, name, tag, index, includeLocation,
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
            @SuppressWarnings("unchecked") private static <T extends Parcelable> List<T> readList(Parcel reply) {
                ParceledListSlice<T> result = reply.readTypedObject(ParceledListSlice.CREATOR);
                return result == null ? null : result.getList();
            }
        }
    }
}
