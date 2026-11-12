/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.nfc;

import android.os.Parcel;
import android.os.Parcelable;

public final class NdefRecord implements Parcelable {
    public static final Creator<NdefRecord> CREATOR = new Creator<NdefRecord>() {
        public NdefRecord createFromParcel(Parcel in) { return new NdefRecord(); }
        public NdefRecord[] newArray(int size) { return new NdefRecord[size]; }
    };

    public NdefRecord(short tnf, byte[] type, byte[] id, byte[] payload) {}
    private NdefRecord() {}

    public short getTnf() { return 0; }
    public byte[] getType() { return new byte[0]; }
    public byte[] getId() { return new byte[0]; }
    public byte[] getPayload() { return new byte[0]; }
    public byte[] toByteArray() { return new byte[0]; }
    public int describeContents() { return 0; }
    public void writeToParcel(Parcel dest, int flags) {}
}
