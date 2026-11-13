/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.nfc;

import android.os.Parcel;
import android.os.Parcelable;

public final class Tag implements Parcelable {
    public static final Creator<Tag> CREATOR = new Creator<Tag>() {
        public Tag createFromParcel(Parcel in) { throw unsupported(); }
        public Tag[] newArray(int size) { return new Tag[size]; }
    };

    // Android tags come from an NFC controller. Keep callback type linkage, but
    // do not fabricate a successful empty tag or silently discard parcel data.
    private Tag() { throw unsupported(); }

    private static UnsupportedOperationException unsupported() {
        return new UnsupportedOperationException("Gesher does not expose NFC tag hardware");
    }

    public byte[] getId() { throw unsupported(); }
    public String[] getTechList() { throw unsupported(); }
    public int describeContents() { return 0; }
    public void writeToParcel(Parcel dest, int flags) { throw unsupported(); }
}
