/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.net;

import android.os.Parcel;
import android.os.Parcelable;

/**
 * The Connectivity module's (deprecated) NetworkInfo (framework.jar compiles against the
 * module's API and does not contain it), with its public API (docs/network.md):
 * {@link ConnectivityManager} makes one per Windows network, connected, of type WIFI,
 * ETHERNET, MOBILE or VPN.
 */
@Deprecated
public class NetworkInfo implements Parcelable {
    public enum State { CONNECTING, CONNECTED, SUSPENDED, DISCONNECTING, DISCONNECTED, UNKNOWN }

    public enum DetailedState {
        IDLE, SCANNING, CONNECTING, AUTHENTICATING, OBTAINING_IPADDR, CONNECTED, SUSPENDED,
        DISCONNECTING, DISCONNECTED, FAILED, BLOCKED, VERIFYING_POOR_LINK, CAPTIVE_PORTAL_CHECK
    }

    private int mNetworkType;
    private int mSubtype;
    private String mTypeName;
    private String mSubtypeName;
    private State mState = State.UNKNOWN;
    private DetailedState mDetailedState = DetailedState.IDLE;
    private String mReason;
    private String mExtraInfo;
    private boolean mIsFailover;
    private boolean mIsAvailable;
    private boolean mIsRoaming;

    public NetworkInfo(int type, int subtype, String typeName, String subtypeName) {
        mNetworkType = type;
        mSubtype = subtype;
        mTypeName = typeName;
        mSubtypeName = subtypeName;
    }

    /** @hide */
    public NetworkInfo(NetworkInfo source) {
        if (source == null) return;
        mNetworkType = source.mNetworkType;
        mSubtype = source.mSubtype;
        mTypeName = source.mTypeName;
        mSubtypeName = source.mSubtypeName;
        mState = source.mState;
        mDetailedState = source.mDetailedState;
        mReason = source.mReason;
        mExtraInfo = source.mExtraInfo;
        mIsFailover = source.mIsFailover;
        mIsAvailable = source.mIsAvailable;
        mIsRoaming = source.mIsRoaming;
    }

    public int getType() {
        return mNetworkType;
    }

    /** @hide */
    public void setType(int type) {
        mNetworkType = type;
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
        return mState == State.CONNECTED || mState == State.CONNECTING;
    }

    public boolean isConnected() {
        return mState == State.CONNECTED;
    }

    /** @hide */
    public boolean isSuspended() {
        return mState == State.SUSPENDED;
    }

    public boolean isAvailable() {
        return mIsAvailable;
    }

    public void setIsAvailable(boolean isAvailable) {
        mIsAvailable = isAvailable;
    }

    public boolean isFailover() {
        return mIsFailover;
    }

    /** @hide */
    public void setFailover(boolean isFailover) {
        mIsFailover = isFailover;
    }

    public boolean isRoaming() {
        return mIsRoaming;
    }

    /** @hide */
    public void setRoaming(boolean isRoaming) {
        mIsRoaming = isRoaming;
    }

    public State getState() {
        return mState;
    }

    public DetailedState getDetailedState() {
        return mDetailedState;
    }

    public void setDetailedState(DetailedState detailedState, String reason, String extraInfo) {
        mDetailedState = detailedState;
        switch (detailedState) {
            case CONNECTED:
                mState = State.CONNECTED;
                break;
            case IDLE:
            case DISCONNECTED:
            case FAILED:
            case BLOCKED:
                mState = State.DISCONNECTED;
                break;
            case SUSPENDED:
                mState = State.SUSPENDED;
                break;
            case DISCONNECTING:
                mState = State.DISCONNECTING;
                break;
            default:
                mState = State.CONNECTING;
                break;
        }
        mReason = reason;
        mExtraInfo = extraInfo;
    }

    /** @hide */
    public void setExtraInfo(String extraInfo) {
        mExtraInfo = extraInfo;
    }

    public String getReason() {
        return mReason;
    }

    public String getExtraInfo() {
        return mExtraInfo;
    }

    @Override
    public String toString() {
        return "[type: " + mTypeName + "[" + mSubtypeName + "], state: " + mState + "/"
                + mDetailedState + ", reason: " + (mReason == null ? "(unspecified)" : mReason)
                + ", extra: " + (mExtraInfo == null ? "(none)" : mExtraInfo)
                + ", failover: " + mIsFailover + ", available: " + mIsAvailable
                + ", roaming: " + mIsRoaming + "]";
    }

    @Override
    public int describeContents() {
        return 0;
    }

    @Override
    public void writeToParcel(Parcel dest, int flags) {
        dest.writeInt(mNetworkType);
        dest.writeInt(mSubtype);
        dest.writeString(mTypeName);
        dest.writeString(mSubtypeName);
        dest.writeString(mState.name());
        dest.writeString(mDetailedState.name());
        dest.writeBoolean(mIsFailover);
        dest.writeBoolean(mIsAvailable);
        dest.writeBoolean(mIsRoaming);
        dest.writeString(mReason);
        dest.writeString(mExtraInfo);
    }

    public static final Creator<NetworkInfo> CREATOR = new Creator<NetworkInfo>() {
        @Override
        public NetworkInfo createFromParcel(Parcel in) {
            NetworkInfo info = new NetworkInfo(in.readInt(), in.readInt(), in.readString(),
                    in.readString());
            info.mState = State.valueOf(in.readString());
            info.mDetailedState = DetailedState.valueOf(in.readString());
            info.mIsFailover = in.readBoolean();
            info.mIsAvailable = in.readBoolean();
            info.mIsRoaming = in.readBoolean();
            info.mReason = in.readString();
            info.mExtraInfo = in.readString();
            return info;
        }

        @Override
        public NetworkInfo[] newArray(int size) {
            return new NetworkInfo[size];
        }
    };
}
