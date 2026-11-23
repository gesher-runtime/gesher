/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.bluetooth;

import android.annotation.IntDef;
import android.content.AttributionSource;
import android.content.Context;
import android.os.Handler;
import android.os.Parcel;
import android.os.ParcelUuid;
import android.os.Parcelable;
import com.gesher.server.BluetoothManagerService;
import com.gesher.server.WindowsBluetooth;
import java.io.IOException;
import java.util.List;
import java.util.UUID;

/**
 * BluetoothDevice with the Bluetooth module's public API over the host radio
 * (docs/bluetooth.md): what Windows knows of the device (name, class, type, bond and SDP
 * service classes of paired devices; names of LE advertisers), GATT connections
 * ({@link BluetoothGatt}) and RFCOMM sockets ({@link BluetoothSocket}). Pairing goes
 * through Windows' own pairing UI ({@link #createBond()}, docs/bluetooth.md).
 *
 * <p>The parcel format (address, address type) is the module's, as ScanResult and the
 * system's broadcasts carry devices.
 */
public final class BluetoothDevice implements Parcelable, Attributable {
    /** @hide Address value annotation used by the AOSP BLE value classes. */
    @IntDef({ADDRESS_TYPE_PUBLIC, ADDRESS_TYPE_RANDOM, ADDRESS_TYPE_ANONYMOUS})
    @java.lang.annotation.Retention(java.lang.annotation.RetentionPolicy.SOURCE)
    public @interface AddressType {}

    public static final int ERROR = Integer.MIN_VALUE;
    public static final String ACTION_FOUND = "android.bluetooth.device.action.FOUND";
    public static final String ACTION_CLASS_CHANGED = "android.bluetooth.device.action.CLASS_CHANGED";
    public static final String ACTION_ACL_CONNECTED = "android.bluetooth.device.action.ACL_CONNECTED";
    public static final String ACTION_ACL_DISCONNECT_REQUESTED =
            "android.bluetooth.device.action.ACL_DISCONNECT_REQUESTED";
    public static final String ACTION_ACL_DISCONNECTED =
            "android.bluetooth.device.action.ACL_DISCONNECTED";
    public static final String ACTION_NAME_CHANGED = "android.bluetooth.device.action.NAME_CHANGED";
    public static final String ACTION_ALIAS_CHANGED = "android.bluetooth.device.action.ALIAS_CHANGED";
    public static final String ACTION_BOND_STATE_CHANGED =
            "android.bluetooth.device.action.BOND_STATE_CHANGED";
    public static final String ACTION_KEY_MISSING = "android.bluetooth.device.action.KEY_MISSING";
    public static final String ACTION_UUID = "android.bluetooth.device.action.UUID";
    public static final String ACTION_PAIRING_REQUEST = "android.bluetooth.device.action.PAIRING_REQUEST";
    public static final String EXTRA_DEVICE = "android.bluetooth.device.extra.DEVICE";
    public static final String EXTRA_NAME = "android.bluetooth.device.extra.NAME";
    public static final String EXTRA_RSSI = "android.bluetooth.device.extra.RSSI";
    public static final String EXTRA_IS_COORDINATED_SET_MEMBER =
            "android.bluetooth.extra.IS_COORDINATED_SET_MEMBER";
    public static final String EXTRA_CLASS = "android.bluetooth.device.extra.CLASS";
    public static final String EXTRA_BOND_STATE = "android.bluetooth.device.extra.BOND_STATE";
    public static final String EXTRA_PREVIOUS_BOND_STATE =
            "android.bluetooth.device.extra.PREVIOUS_BOND_STATE";
    public static final String EXTRA_PAIRING_VARIANT = "android.bluetooth.device.extra.PAIRING_VARIANT";
    public static final String EXTRA_PAIRING_KEY = "android.bluetooth.device.extra.PAIRING_KEY";
    public static final String EXTRA_UUID = "android.bluetooth.device.extra.UUID";
    public static final String EXTRA_TRANSPORT = "android.bluetooth.device.extra.TRANSPORT";
    public static final int BOND_NONE = 10;
    public static final int BOND_BONDING = 11;
    public static final int BOND_BONDED = 12;
    public static final int DEVICE_TYPE_UNKNOWN = 0;
    public static final int DEVICE_TYPE_CLASSIC = 1;
    public static final int DEVICE_TYPE_LE = 2;
    public static final int DEVICE_TYPE_DUAL = 3;
    public static final int PAIRING_VARIANT_PIN = 0;
    public static final int PAIRING_VARIANT_PASSKEY_CONFIRMATION = 2;
    public static final int TRANSPORT_AUTO = 0;
    public static final int TRANSPORT_BREDR = 1;
    public static final int TRANSPORT_LE = 2;
    public static final int PHY_LE_1M = 1;
    public static final int PHY_LE_2M = 2;
    public static final int PHY_LE_CODED = 3;
    public static final int PHY_LE_1M_MASK = 1;
    public static final int PHY_LE_2M_MASK = 2;
    public static final int PHY_LE_CODED_MASK = 4;
    public static final int PHY_OPTION_NO_PREFERRED = 0;
    public static final int PHY_OPTION_S2 = 1;
    public static final int PHY_OPTION_S8 = 2;
    public static final int ADDRESS_TYPE_PUBLIC = 0;
    public static final int ADDRESS_TYPE_RANDOM = 1;
    public static final int ADDRESS_TYPE_UNKNOWN = 0xFFFF;
    public static final int ADDRESS_TYPE_ANONYMOUS = 0xFF;
    /** @hide */
    public static final int CONNECTION_STATE_DISCONNECTED = 0;
    /** @hide */
    public static final int CONNECTION_STATE_CONNECTED = 1;

    private final String mAddress;
    private final int mAddressType;
    private AttributionSource mAttributionSource;
    private transient Context mContext;

    BluetoothDevice(String address, int addressType) {
        if (!BluetoothAdapter.checkBluetoothAddress(address)) {
            throw new IllegalArgumentException(address + " is not a valid Bluetooth address");
        }
        if (addressType != ADDRESS_TYPE_PUBLIC && addressType != ADDRESS_TYPE_RANDOM
                && addressType != ADDRESS_TYPE_ANONYMOUS) {
            throw new IllegalArgumentException(addressType + " is not a Bluetooth address type");
        }
        mAddress = address;
        mAddressType = addressType;
    }

    private BluetoothDevice(Parcel in) {
        this(in.readString(), in.readInt());
    }

    /** @hide For the system server's broadcasts (com.gesher.server.BluetoothManagerService). */
    public static BluetoothDevice createFromHost(String address, int addressType) {
        return new BluetoothDevice(address, addressType);
    }

    /** The adapter's (or manager's) context, for permission checks. */
    BluetoothDevice withContext(Context context) {
        mContext = context;
        return this;
    }

    Context context() {
        return mContext;
    }

    long value() {
        return WindowsBluetooth.address(mAddress);
    }

    public String getAddress() {
        return mAddress;
    }

    /** @hide */
    public String getAnonymizedAddress() {
        return "XX:XX:XX:XX" + mAddress.substring(11);
    }

    public int getAddressType() {
        return mAddressType;
    }

    /** @hide Attribution for the value, without starting any Bluetooth service. */
    @Override
    public void setAttributionSource(AttributionSource source) {
        mAttributionSource = source;
    }

    /**
     * @hide As AOSP's (android-16.0.0_r4): framework.jar's {@code Intent.prepareToEnterProcess}
     * calls it on a Bluetooth broadcast's {@link #EXTRA_DEVICE} before the receiver runs, to
     * attribute the unparceled device to the receiving app (issue #303).
     */
    public void prepareToEnterProcess(AttributionSource attributionSource) {
        setAttributionSource(attributionSource);
    }

    @Override
    public int describeContents() {
        return 0;
    }

    @Override
    public void writeToParcel(Parcel out, int flags) {
        out.writeString(mAddress);
        out.writeInt(mAddressType);
    }

    public static final Parcelable.Creator<BluetoothDevice> CREATOR =
            new Parcelable.Creator<BluetoothDevice>() {
                @Override
                public BluetoothDevice createFromParcel(Parcel in) {
                    return new BluetoothDevice(in);
                }

                @Override
                public BluetoothDevice[] newArray(int size) {
                    return new BluetoothDevice[size];
                }
            };

    @Override
    public boolean equals(Object other) {
        return other instanceof BluetoothDevice
                && mAddress.equals(((BluetoothDevice) other).mAddress);
    }

    @Override
    public int hashCode() {
        return mAddress.hashCode();
    }

    @Override
    public String toString() {
        return mAddress;
    }

    // --- what Windows knows ----------------------------------------------------------------

    public String getName() {
        BluetoothHost.enforceConnect(mContext);
        if (!BluetoothHost.present()) return null;
        return BluetoothHost.known(mAddress).name;
    }

    public String getAlias() {
        return getName();
    }

    /** Windows owns device names: the alias cannot be set. */
    public int setAlias(String alias) {
        BluetoothHost.enforceConnect(mContext);
        return BluetoothStatusCodes.ERROR_DEVICE_NOT_BONDED;
    }

    public int getType() {
        BluetoothHost.enforceConnect(mContext);
        int kind = BluetoothHost.known(mAddress).kind;
        boolean classic = (kind & WindowsBluetooth.KIND_CLASSIC) != 0;
        boolean le = (kind & WindowsBluetooth.KIND_LE) != 0;
        return classic && le ? DEVICE_TYPE_DUAL : classic ? DEVICE_TYPE_CLASSIC
                : le ? DEVICE_TYPE_LE : DEVICE_TYPE_UNKNOWN;
    }

    public BluetoothClass getBluetoothClass() {
        BluetoothHost.enforceConnect(mContext);
        BluetoothHost.Known known = BluetoothHost.known(mAddress);
        return known.classOfDevice == 0 && !known.bonded ? null
                : new BluetoothClass(known.classOfDevice);
    }

    public int getBondState() {
        BluetoothHost.enforceConnect(mContext);
        return BluetoothHost.known(mAddress).bonded ? BOND_BONDED : BOND_NONE;
    }

    /** @hide Hidden in the module, called through reflection by many apps. */
    public boolean isConnected() {
        BluetoothHost.enforceConnect(mContext);
        return BluetoothHost.known(mAddress).connected || BluetoothGatt.isConnected(mAddress);
    }

    /**
     * @hide Encryption Gesher can vouch for: a secure RFCOMM socket of this process is
     * connected to the device (Windows opened it requiring authentication and encryption).
     * Windows reports no link encryption otherwise (ACL or GATT), so other links answer false.
     */
    public boolean isEncrypted() {
        BluetoothHost.enforceConnect(mContext);
        return BluetoothSocket.encryptedLink(mAddress);
    }

    public ParcelUuid[] getUuids() {
        BluetoothHost.enforceConnect(mContext);
        List<UUID> uuids = BluetoothHost.known(mAddress).uuids;
        if (uuids.isEmpty()) return null;
        ParcelUuid[] out = new ParcelUuid[uuids.size()];
        for (int i = 0; i < out.length; i++) out[i] = new ParcelUuid(uuids.get(i));
        return out;
    }

    /** Asks the device for its SDP records; ACTION_UUID follows. */
    public boolean fetchUuidsWithSdp() {
        return fetchUuidsWithSdp(TRANSPORT_AUTO);
    }

    /** @hide */
    public boolean fetchUuidsWithSdp(int transport) {
        BluetoothHost.enforceConnect(mContext);
        if (!BluetoothHost.on()) return false;
        if (BluetoothManagerService.Client.available()) {
            return BluetoothManagerService.Client.fetchUuids(mAddress);
        }
        // No system server: refresh what this process knows; no broadcast to send.
        BluetoothHost.sawUuids(mAddress, WindowsBluetooth.remoteUuids(value(), true));
        return true;
    }

    /**
     * Pairs through Windows' own pairing UI (the user confirms there): true when it
     * started; ACTION_BOND_STATE_CHANGED reports BONDING, then BONDED or NONE. False while
     * the radio is off, for a bonded device or with a pairing running.
     */
    public boolean createBond() {
        return createBond(getType() == DEVICE_TYPE_LE ? TRANSPORT_LE : TRANSPORT_AUTO);
    }

    /** @hide */
    public boolean createBond(int transport) {
        BluetoothHost.enforceAdmin(mContext);
        if (!BluetoothHost.on() || !BluetoothManagerService.Client.available()) return false;
        boolean le = transport == TRANSPORT_LE
                || (transport == TRANSPORT_AUTO && mAddressType == ADDRESS_TYPE_RANDOM);
        return BluetoothManagerService.Client.createBond(mAddress, le);
    }

    /** @hide Removes Windows' pairing (the user confirms). */
    public boolean removeBond() {
        BluetoothHost.enforceAdmin(mContext);
        if (!BluetoothHost.on() || !BluetoothManagerService.Client.available()) return false;
        return BluetoothManagerService.Client.removeBond(mAddress);
    }

    /** @hide */
    public boolean cancelBondProcess() {
        return false;
    }

    public boolean setPin(byte[] pin) {
        BluetoothHost.enforceConnect(mContext);
        return false;
    }

    public boolean setPairingConfirmation(boolean confirm) {
        BluetoothHost.enforceConnect(mContext);
        return false;
    }

    // --- GATT --------------------------------------------------------------------------------

    public BluetoothGatt connectGatt(Context context, boolean autoConnect,
            BluetoothGattCallback callback) {
        return connectGatt(context, autoConnect, callback, TRANSPORT_AUTO);
    }

    public BluetoothGatt connectGatt(Context context, boolean autoConnect,
            BluetoothGattCallback callback, int transport) {
        return connectGatt(context, autoConnect, callback, transport, PHY_LE_1M_MASK);
    }

    public BluetoothGatt connectGatt(Context context, boolean autoConnect,
            BluetoothGattCallback callback, int transport, int phy) {
        return connectGatt(context, autoConnect, callback, transport, phy, null);
    }

    public BluetoothGatt connectGatt(Context context, boolean autoConnect,
            BluetoothGattCallback callback, int transport, int phy, Handler handler) {
        return connectGatt(context, autoConnect, callback, transport, false, phy, handler);
    }

    /** @hide */
    public BluetoothGatt connectGatt(Context context, boolean autoConnect,
            BluetoothGattCallback callback, int transport, boolean opportunistic, int phy,
            Handler handler) {
        if (callback == null) throw new NullPointerException("callback is null");
        Context checked = context != null ? context : mContext;
        BluetoothHost.enforceConnect(checked);
        if (!BluetoothHost.present()) return null;
        BluetoothGatt gatt = new BluetoothGatt(this, checked, callback, handler, autoConnect);
        gatt.connectAsync();
        return gatt;
    }

    // --- RFCOMM and L2CAP ---------------------------------------------------------------

    public BluetoothSocket createRfcommSocketToServiceRecord(UUID uuid) throws IOException {
        if (uuid == null) throw new NullPointerException("uuid is null");
        BluetoothHost.enforceConnect(mContext);
        if (!BluetoothHost.on()) throw new IOException("Bluetooth is off");
        return new BluetoothSocket(this, uuid, -1, true);
    }

    public BluetoothSocket createInsecureRfcommSocketToServiceRecord(UUID uuid) throws IOException {
        if (uuid == null) throw new NullPointerException("uuid is null");
        BluetoothHost.enforceConnect(mContext);
        if (!BluetoothHost.on()) throw new IOException("Bluetooth is off");
        return new BluetoothSocket(this, uuid, -1, false);
    }

    /** @hide Hidden in the module, called through reflection by many SPP apps. */
    public BluetoothSocket createRfcommSocket(int channel) throws IOException {
        BluetoothHost.enforceConnect(mContext);
        if (!BluetoothHost.on()) throw new IOException("Bluetooth is off");
        if (channel < 1 || channel > 30) throw new IOException("Invalid RFCOMM channel: " + channel);
        return new BluetoothSocket(this, null, channel, true);
    }

    /** @hide */
    public BluetoothSocket createInsecureRfcommSocket(int channel) throws IOException {
        BluetoothHost.enforceConnect(mContext);
        if (!BluetoothHost.on()) throw new IOException("Bluetooth is off");
        if (channel < 1 || channel > 30) throw new IOException("Invalid RFCOMM channel: " + channel);
        return new BluetoothSocket(this, null, channel, false);
    }

    /** Windows offers applications no L2CAP channels. */
    public BluetoothSocket createL2capChannel(int psm) throws IOException {
        BluetoothHost.enforceConnect(mContext);
        throw new IOException("L2CAP channels are not available on this device");
    }

    public BluetoothSocket createInsecureL2capChannel(int psm) throws IOException {
        return createL2capChannel(psm);
    }
}
