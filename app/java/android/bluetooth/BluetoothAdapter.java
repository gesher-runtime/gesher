/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.bluetooth;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.bluetooth.le.BluetoothLeAdvertiser;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanFilter;
import android.bluetooth.le.ScanRecord;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.os.Build;
import android.os.ParcelUuid;
import com.gesher.server.BluetoothManagerService;
import com.gesher.server.WindowsBluetooth;
import java.io.IOException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.UUID;

/**
 * BluetoothAdapter with the Bluetooth module's public API over the host's radio
 * (docs/bluetooth.md). There is an adapter when Windows has a Bluetooth radio (or with
 * {@code GESHER_BLUETOOTH_TEST=1}); without one, {@link #getDefaultAdapter()} and
 * BluetoothManager#getAdapter() are null, as on a device without Bluetooth.
 *
 * <p>State is Windows' radio (on or off); enable/disable ask Windows to switch it (apps
 * targeting API 33+ get false, as on Android). Paired devices are Windows' pairings; the
 * local address is hidden from apps ({@link #DEFAULT_MAC_ADDRESS}), as on Android.
 * Permissions are the module's: BLUETOOTH_CONNECT / BLUETOOTH_SCAN on API 31+, BLUETOOTH /
 * BLUETOOTH_ADMIN below.
 */
public final class BluetoothAdapter {
    private static final String TAG = "BluetoothAdapter";

    public static final String DEFAULT_MAC_ADDRESS = "02:00:00:00:00:00";
    public static final int ERROR = Integer.MIN_VALUE;
    public static final String ACTION_STATE_CHANGED = "android.bluetooth.adapter.action.STATE_CHANGED";
    public static final String EXTRA_STATE = "android.bluetooth.adapter.extra.STATE";
    public static final String EXTRA_PREVIOUS_STATE = "android.bluetooth.adapter.extra.PREVIOUS_STATE";
    public static final int STATE_OFF = 10;
    public static final int STATE_TURNING_ON = 11;
    public static final int STATE_ON = 12;
    public static final int STATE_TURNING_OFF = 13;
    /** @hide */
    public static final int STATE_BLE_TURNING_ON = 14;
    /** @hide */
    public static final int STATE_BLE_ON = 15;
    /** @hide */
    public static final int STATE_BLE_TURNING_OFF = 16;
    public static final String ACTION_REQUEST_DISCOVERABLE =
            "android.bluetooth.adapter.action.REQUEST_DISCOVERABLE";
    public static final String EXTRA_DISCOVERABLE_DURATION =
            "android.bluetooth.adapter.extra.DISCOVERABLE_DURATION";
    public static final String ACTION_REQUEST_ENABLE = "android.bluetooth.adapter.action.REQUEST_ENABLE";
    public static final String ACTION_REQUEST_DISABLE =
            "android.bluetooth.adapter.action.REQUEST_DISABLE";
    public static final String ACTION_SCAN_MODE_CHANGED =
            "android.bluetooth.adapter.action.SCAN_MODE_CHANGED";
    public static final String EXTRA_SCAN_MODE = "android.bluetooth.adapter.extra.SCAN_MODE";
    public static final String EXTRA_PREVIOUS_SCAN_MODE =
            "android.bluetooth.adapter.extra.PREVIOUS_SCAN_MODE";
    public static final int SCAN_MODE_NONE = 20;
    public static final int SCAN_MODE_CONNECTABLE = 21;
    public static final int SCAN_MODE_CONNECTABLE_DISCOVERABLE = 23;
    public static final String ACTION_DISCOVERY_STARTED =
            "android.bluetooth.adapter.action.DISCOVERY_STARTED";
    public static final String ACTION_DISCOVERY_FINISHED =
            "android.bluetooth.adapter.action.DISCOVERY_FINISHED";
    public static final String ACTION_LOCAL_NAME_CHANGED =
            "android.bluetooth.adapter.action.LOCAL_NAME_CHANGED";
    public static final String EXTRA_LOCAL_NAME = "android.bluetooth.adapter.extra.LOCAL_NAME";
    public static final String ACTION_CONNECTION_STATE_CHANGED =
            "android.bluetooth.adapter.action.CONNECTION_STATE_CHANGED";
    public static final String EXTRA_CONNECTION_STATE =
            "android.bluetooth.adapter.extra.CONNECTION_STATE";
    public static final String EXTRA_PREVIOUS_CONNECTION_STATE =
            "android.bluetooth.adapter.extra.PREVIOUS_CONNECTION_STATE";
    public static final int STATE_DISCONNECTED = 0;
    public static final int STATE_CONNECTING = 1;
    public static final int STATE_CONNECTED = 2;
    public static final int STATE_DISCONNECTING = 3;

    private static BluetoothAdapter sAdapter;

    private final Context mContext;
    private BluetoothLeScanner mScanner;
    private final Map<LeScanCallback, ScanCallback> mLeScans = new HashMap<>();

    BluetoothAdapter(Context context) {
        mContext = context;
        // Start the system's event watch even for apps that only read adapter/device
        // state. Those apps still need broadcasts when Windows changes the radio or
        // a paired device connects; they need not start discovery or pairing first.
        BluetoothManagerService.Client.state();
    }

    /** @deprecated as in the module: use BluetoothManager#getAdapter(). */
    @Deprecated
    public static synchronized BluetoothAdapter getDefaultAdapter() {
        if (!BluetoothHost.present()) return null;
        if (sAdapter == null) sAdapter = new BluetoothAdapter(null);
        return sAdapter;
    }

    /** @hide */
    public static BluetoothAdapter createAdapter(Context context) {
        return BluetoothHost.present() ? new BluetoothAdapter(context) : null;
    }

    Context context() {
        return mContext;
    }

    // --- state -----------------------------------------------------------------------------

    public boolean isEnabled() {
        return getState() == STATE_ON;
    }

    public int getState() {
        return BluetoothHost.on() ? STATE_ON : STATE_OFF;
    }

    /** @hide BLE stays on with the radio. */
    public boolean isLeEnabled() {
        return BluetoothHost.on();
    }

    /** @hide */
    public int getLeState() {
        return getState();
    }

    /** Asks Windows to turn the radio on (apps targeting API 33+ get false, as on Android). */
    @Deprecated
    public boolean enable() {
        BluetoothHost.enforceAdmin(mContext);
        if (BluetoothHost.targetSdk(BluetoothHost.context(mContext)) >= Build.VERSION_CODES.TIRAMISU) {
            return false;
        }
        if (isEnabled()) return true;
        boolean ok = BluetoothManagerService.Client.setEnabled(true);
        WindowsBluetooth.invalidate();
        return ok;
    }

    @Deprecated
    public boolean disable() {
        BluetoothHost.enforceAdmin(mContext);
        if (BluetoothHost.targetSdk(BluetoothHost.context(mContext)) >= Build.VERSION_CODES.TIRAMISU) {
            return false;
        }
        if (!isEnabled()) return true;
        boolean ok = BluetoothManagerService.Client.setEnabled(false);
        WindowsBluetooth.invalidate();
        return ok;
    }

    /** The PC's Bluetooth name (its computer name, as Windows advertises it). */
    public String getName() {
        BluetoothHost.enforceConnect(mContext);
        WindowsBluetooth.Adapter a = BluetoothHost.adapter();
        return a.present ? a.name : null;
    }

    /** Windows owns the name. */
    public boolean setName(String name) {
        BluetoothHost.enforceConnect(mContext);
        return false;
    }

    /** Apps get {@link #DEFAULT_MAC_ADDRESS}, as on Android (LOCAL_MAC_ADDRESS is privileged). */
    public String getAddress() {
        BluetoothHost.enforceConnect(mContext);
        return DEFAULT_MAC_ADDRESS;
    }

    public int getScanMode() {
        BluetoothHost.enforceScan(mContext);
        return isEnabled() ? SCAN_MODE_CONNECTABLE : SCAN_MODE_NONE;
    }

    // --- devices ---------------------------------------------------------------------------

    public Set<BluetoothDevice> getBondedDevices() {
        BluetoothHost.enforceConnect(mContext);
        Set<BluetoothDevice> out = new HashSet<>();
        if (!isEnabled()) return out;
        for (WindowsBluetooth.Device d : BluetoothHost.bonded()) {
            out.add(BluetoothHost.device(WindowsBluetooth.address(d.address), d.addressType, mContext));
        }
        return out;
    }

    public BluetoothDevice getRemoteDevice(String address) {
        return new BluetoothDevice(address, BluetoothDevice.ADDRESS_TYPE_PUBLIC).withContext(mContext);
    }

    public BluetoothDevice getRemoteDevice(byte[] address) {
        if (address == null || address.length != 6) {
            throw new IllegalArgumentException("Bluetooth address must have 6 bytes");
        }
        return getRemoteDevice(String.format(java.util.Locale.ROOT, "%02X:%02X:%02X:%02X:%02X:%02X",
                address[0], address[1], address[2], address[3], address[4], address[5]));
    }

    public BluetoothDevice getRemoteLeDevice(String address, int addressType) {
        if (addressType != BluetoothDevice.ADDRESS_TYPE_PUBLIC
                && addressType != BluetoothDevice.ADDRESS_TYPE_RANDOM) {
            throw new IllegalArgumentException(addressType + " is not a Bluetooth LE address type");
        }
        return new BluetoothDevice(address, addressType).withContext(mContext);
    }

    public boolean startDiscovery() {
        BluetoothHost.enforceScan(mContext);
        if (!isEnabled()) return false;
        return BluetoothManagerService.Client.startDiscovery();
    }

    public boolean cancelDiscovery() {
        BluetoothHost.enforceScan(mContext);
        if (!isEnabled()) return false;
        return BluetoothManagerService.Client.cancelDiscovery();
    }

    public boolean isDiscovering() {
        BluetoothHost.enforceScan(mContext);
        return isEnabled() && BluetoothManagerService.Client.isDiscovering();
    }

    // --- LE ------------------------------------------------------------------------------

    public BluetoothLeScanner getBluetoothLeScanner() {
        if (!isLeEnabled()) return null;
        synchronized (this) {
            if (mScanner == null) mScanner = new BluetoothLeScanner(mContext);
            return mScanner;
        }
    }

    /** Android returns null when LE advertising is unavailable on this device. */
    public BluetoothLeAdvertiser getBluetoothLeAdvertiser() {
        return null;
    }

    public boolean isMultipleAdvertisementSupported() {
        return false;
    }

    public boolean isOffloadedFilteringSupported() {
        return false;
    }

    /** Whether the controller provides on-chip scan batching, not app-process batching. */
    public boolean isOffloadedScanBatchingSupported() {
        // The Windows advertisement watcher does not expose controller batching.
        return false;
    }

    public boolean isLe2MPhySupported() {
        return BluetoothHost.adapter().le2mPhy();
    }

    public boolean isLeCodedPhySupported() {
        return BluetoothHost.adapter().leCodedPhy();
    }

    public boolean isLeExtendedAdvertisingSupported() {
        // The host controller may support it, but Gesher currently exposes only
        // legacy advertisement records and does not implement extended advertising.
        return false;
    }

    public boolean isLePeriodicAdvertisingSupported() {
        return false;
    }

    public int getLeMaximumAdvertisingDataLength() {
        // Android reports zero when extended advertising is not supported.
        return 0;
    }

    public int getMaxConnectedAudioDevices() {
        return 1;
    }

    public int isLeAudioSupported() {
        return BluetoothStatusCodes.FEATURE_NOT_SUPPORTED;
    }

    public int isLeAudioBroadcastSourceSupported() {
        return BluetoothStatusCodes.FEATURE_NOT_SUPPORTED;
    }

    public int isLeAudioBroadcastAssistantSupported() {
        return BluetoothStatusCodes.FEATURE_NOT_SUPPORTED;
    }

    /** No controller can offload transport discovery filtering. */
    public int getOffloadedTransportDiscoveryDataScanSupported() {
        return BluetoothStatusCodes.FEATURE_NOT_SUPPORTED;
    }

    /** The legacy LE scan callback. */
    public interface LeScanCallback {
        void onLeScan(BluetoothDevice device, int rssi, byte[] scanRecord);
    }

    @Deprecated
    public boolean startLeScan(LeScanCallback callback) {
        return startLeScan(null, callback);
    }

    @Deprecated
    public boolean startLeScan(UUID[] serviceUuids, LeScanCallback callback) {
        if (callback == null) return false;
        BluetoothLeScanner scanner = getBluetoothLeScanner();
        if (scanner == null) return false;
        synchronized (mLeScans) {
            if (mLeScans.containsKey(callback)) return false;
            ScanCallback wrapper = new ScanCallback() {
                @Override
                public void onScanResult(int callbackType, ScanResult result) {
                    ScanRecord record = result.getScanRecord();
                    if (serviceUuids != null && serviceUuids.length > 0) {
                        List<ParcelUuid> offered = record == null ? null : record.getServiceUuids();
                        if (offered == null) return;
                        for (UUID uuid : serviceUuids) {
                            if (!offered.contains(new ParcelUuid(uuid))) return;
                        }
                    }
                    callback.onLeScan(result.getDevice(), result.getRssi(),
                            record == null ? new byte[0] : record.getBytes());
                }
            };
            List<ScanFilter> filters = new ArrayList<>();
            scanner.startScan(filters, new ScanSettings.Builder()
                    .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build(), wrapper);
            mLeScans.put(callback, wrapper);
        }
        return true;
    }

    @Deprecated
    public void stopLeScan(LeScanCallback callback) {
        ScanCallback wrapper;
        synchronized (mLeScans) {
            wrapper = mLeScans.remove(callback);
        }
        BluetoothLeScanner scanner = getBluetoothLeScanner();
        if (wrapper != null && scanner != null) scanner.stopScan(wrapper);
    }

    // --- profiles ----------------------------------------------------------------------

    /** A2DP and headset proxies; other profiles are not available. */
    public boolean getProfileProxy(Context context, BluetoothProfile.ServiceListener listener,
            int profile) {
        if (context == null || listener == null) return false;
        final BluetoothProfile proxy;
        switch (profile) {
            case BluetoothProfile.A2DP:
                proxy = new BluetoothA2dp(context);
                break;
            case BluetoothProfile.HEADSET:
                proxy = new BluetoothHeadset(context);
                break;
            default:
                Log.i(TAG, "profile " + profile + " is not available");
                return false;
        }
        Looper looper = Looper.getMainLooper();
        if (looper == null) {
            listener.onServiceConnected(profile, proxy);
        } else {
            new Handler(looper).post(() -> listener.onServiceConnected(profile, proxy));
        }
        return true;
    }

    public void closeProfileProxy(int profile, BluetoothProfile proxy) {
        // Nothing is bound: the proxies read Windows' state when asked.
    }

    public int getProfileConnectionState(int profile) {
        BluetoothHost.enforceConnect(mContext);
        if (!isEnabled()) return BluetoothProfile.STATE_DISCONNECTED;
        for (WindowsBluetooth.Device d : BluetoothHost.bonded()) {
            if (!d.connected) continue;
            if (profile == BluetoothProfile.A2DP && BluetoothAudioProfile.isA2dp(d)) {
                return BluetoothProfile.STATE_CONNECTED;
            }
            if (profile == BluetoothProfile.HEADSET && BluetoothAudioProfile.isHeadset(d)) {
                return BluetoothProfile.STATE_CONNECTED;
            }
        }
        return BluetoothProfile.STATE_DISCONNECTED;
    }

    // --- RFCOMM and L2CAP listeners ------------------------------------------------------

    public BluetoothServerSocket listenUsingRfcommWithServiceRecord(String name, UUID uuid)
            throws IOException {
        return listen(name, uuid, true);
    }

    public BluetoothServerSocket listenUsingInsecureRfcommWithServiceRecord(String name, UUID uuid)
            throws IOException {
        return listen(name, uuid, false);
    }

    private BluetoothServerSocket listen(String name, UUID uuid, boolean secure)
            throws IOException {
        if (uuid == null) throw new NullPointerException("uuid is null");
        BluetoothHost.enforceConnect(mContext);
        if (!isEnabled()) throw new IOException("Bluetooth is off");
        return BluetoothServerSocket.listen(mContext, name, uuid, secure);
    }

    public BluetoothServerSocket listenUsingL2capChannel() throws IOException {
        BluetoothHost.enforceConnect(mContext);
        throw new IOException("L2CAP channels are not available on this device");
    }

    public BluetoothServerSocket listenUsingInsecureL2capChannel() throws IOException {
        return listenUsingL2capChannel();
    }

    // --- address helpers ---------------------------------------------------------------

    /** @hide AOSP address classification, independent of a controller. */
    public static boolean isAddressRandomStatic(String address) {
        java.util.Objects.requireNonNull(address);
        return checkBluetoothAddress(address)
                && (Integer.parseInt(address.split(":")[0], 16) & 0xC0) == 0xC0;
    }

    public static boolean checkBluetoothAddress(String address) {
        if (address == null || address.length() != 17) return false;
        for (int i = 0; i < 17; i++) {
            char c = address.charAt(i);
            if (i % 3 == 2) {
                if (c != ':') return false;
            } else if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) {
                return false;
            }
        }
        return true;
    }
}
