// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
package android.os;
public interface Parcelable {
    interface Creator<T> { T createFromParcel(Parcel source); }
}
