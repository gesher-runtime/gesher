/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.net.wifi;

import android.os.Parcel;
import android.os.Parcelable;

/** API 36 scan data fields. Gesher's unsupported Wi-Fi manager returns no scan entries. */
public class ScanResult implements Parcelable {
    public String SSID;
    public String BSSID;
    public String capabilities;
    public int frequency;
    public int level;
    public long timestamp;
    public ScanResult() { }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel out, int flags) {
        out.writeString(SSID);
        out.writeString(BSSID);
        out.writeString(capabilities);
        out.writeInt(frequency);
        out.writeInt(level);
        out.writeLong(timestamp);
    }
    public static final Creator<ScanResult> CREATOR = new Creator<>() {
        @Override public ScanResult createFromParcel(Parcel in) {
            ScanResult sr = new ScanResult();
            sr.SSID = in.readString();
            sr.BSSID = in.readString();
            sr.capabilities = in.readString();
            sr.frequency = in.readInt();
            sr.level = in.readInt();
            sr.timestamp = in.readLong();
            return sr;
        }
        @Override public ScanResult[] newArray(int size) { return new ScanResult[size]; }
    };
}
