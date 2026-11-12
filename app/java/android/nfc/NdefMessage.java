/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.nfc;

import android.os.Parcel;
import android.os.Parcelable;

public final class NdefMessage implements Parcelable {
    public static final Creator<NdefMessage> CREATOR = new Creator<NdefMessage>() {
        public NdefMessage createFromParcel(Parcel in) { return new NdefMessage(); }
        public NdefMessage[] newArray(int size) { return new NdefMessage[size]; }
    };

    public NdefMessage(NdefRecord[] records) {}
    public NdefMessage(byte[] data) {}
    private NdefMessage() {}

    public NdefRecord[] getRecords() { return new NdefRecord[0]; }
    public int getByteArrayLength() { return 0; }
    public byte[] toByteArray() { return new byte[0]; }
    public int describeContents() { return 0; }
    public void writeToParcel(Parcel dest, int flags) {}
}
