/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.net;

/**
 * A placeholder for the Connectivity module's (deprecated) NetworkInfo, for
 * {@link ConnectivityManager}'s signatures: with no network there is none, and one made
 * by an app is disconnected.
 */
@Deprecated
public class NetworkInfo {
    public enum State { CONNECTING, CONNECTED, SUSPENDED, DISCONNECTING, DISCONNECTED, UNKNOWN }

    public enum DetailedState {
        IDLE, SCANNING, CONNECTING, AUTHENTICATING, OBTAINING_IPADDR, CONNECTED, SUSPENDED,
        DISCONNECTING, DISCONNECTED, FAILED, BLOCKED, VERIFYING_POOR_LINK, CAPTIVE_PORTAL_CHECK
    }

    private final int mType;
    private final int mSubtype;
    private final String mTypeName;
    private final String mSubtypeName;

    public NetworkInfo(int type, int subtype, String typeName, String subtypeName) {
        mType = type;
        mSubtype = subtype;
        mTypeName = typeName;
        mSubtypeName = subtypeName;
    }

    public int getType() {
        return mType;
    }

    public int getSubtype() {
        return mSubtype;
    }

    public String getTypeName() {
        return mTypeName;
    }

    public String getSubtypeName() {
        return mSubtypeName;
    }

    public boolean isConnectedOrConnecting() {
        return false;
    }

    public boolean isConnected() {
        return false;
    }

    public boolean isAvailable() {
        return false;
    }

    public boolean isFailover() {
        return false;
    }

    public boolean isRoaming() {
        return false;
    }

    public State getState() {
        return State.DISCONNECTED;
    }

    public DetailedState getDetailedState() {
        return DetailedState.DISCONNECTED;
    }

    public String getReason() {
        return null;
    }

    public String getExtraInfo() {
        return null;
    }

    @Override
    public String toString() {
        return "[type: " + mTypeName + "[" + mSubtypeName + "], state: DISCONNECTED]";
    }
}
