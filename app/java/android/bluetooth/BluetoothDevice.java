/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.bluetooth;

import android.annotation.IntDef;
import android.content.AttributionSource;
import android.os.Parcel;
import android.os.Parcelable;

/**
 * BluetoothDevice for a device without Bluetooth: devices come from an adapter
 * or a profile, so no hardware device is produced here. Its address value can
 * still cross a Parcel, as the Bluetooth module's ScanResult requires.
 */
public final class BluetoothDevice implements Parcelable, Attributable {
    /** @hide Address value annotation used by the AOSP BLE value classes. */
    @IntDef({ADDRESS_TYPE_PUBLIC, ADDRESS_TYPE_RANDOM, ADDRESS_TYPE_ANONYMOUS})
    @java.lang.annotation.Retention(java.lang.annotation.RetentionPolicy.SOURCE)
    public @interface AddressType {}
    public static final String ACTION_FOUND = "android.bluetooth.device.action.FOUND";
    public static final String ACTION_ACL_CONNECTED = "android.bluetooth.device.action.ACL_CONNECTED";
    public static final String ACTION_ACL_DISCONNECTED =
            "android.bluetooth.device.action.ACL_DISCONNECTED";
    public static final String ACTION_BOND_STATE_CHANGED =
            "android.bluetooth.device.action.BOND_STATE_CHANGED";
    public static final String EXTRA_DEVICE = "android.bluetooth.device.extra.DEVICE";
    public static final int BOND_NONE = 10;
    public static final int BOND_BONDING = 11;
    public static final int BOND_BONDED = 12;
    public static final int DEVICE_TYPE_UNKNOWN = 0;
    public static final int DEVICE_TYPE_CLASSIC = 1;
    public static final int DEVICE_TYPE_LE = 2;
    public static final int DEVICE_TYPE_DUAL = 3;
    public static final int PHY_LE_1M = 1;
    public static final int PHY_LE_2M = 2;
    public static final int PHY_LE_CODED = 3;
    public static final int ADDRESS_TYPE_PUBLIC = 0;
    public static final int ADDRESS_TYPE_RANDOM = 1;
    public static final int ADDRESS_TYPE_ANONYMOUS = 0xFF;

    private final String mAddress;
    private final int mAddressType;
    private AttributionSource mAttributionSource;

    private BluetoothDevice(Parcel in) {
        String address = in.readString();
        int addressType = in.readInt();
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

    public String getAddress() {
        return mAddress;
    }

    public int getAddressType() {
        return mAddressType;
    }

    /** @hide Attribution for the value, without starting any Bluetooth service. */
    @Override
    public void setAttributionSource(AttributionSource source) {
        mAttributionSource = source;
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

    public String getName() {
        return null;
    }

    public int getType() {
        return DEVICE_TYPE_UNKNOWN;
    }

    public int getBondState() {
        return BOND_NONE;
    }
}
