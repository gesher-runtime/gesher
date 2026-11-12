/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.nfc;

import android.os.Parcel;
import android.os.Parcelable;

public final class Tag implements Parcelable {
    public static final Creator<Tag> CREATOR = new Creator<Tag>() {
        public Tag createFromParcel(Parcel in) { return new Tag(); }
        public Tag[] newArray(int size) { return new Tag[size]; }
    };

    public Tag() {}

    public byte[] getId() { return new byte[0]; }
    public String[] getTechList() { return new String[0]; }
    public int describeContents() { return 0; }
    public void writeToParcel(Parcel dest, int flags) {}
}
