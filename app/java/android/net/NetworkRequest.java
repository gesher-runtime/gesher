/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.net;

import android.os.Parcel;
import android.os.Parcelable;

import java.util.Objects;

/**
 * The Connectivity module's NetworkRequest (framework.jar compiles against the module's
 * API and does not contain it), with its public API (docs/network.md): the capabilities
 * and transports a network must have, as {@link ConnectivityManager} matches Windows'
 * networks against them. A new Builder asks for NOT_RESTRICTED, TRUSTED and NOT_VPN, as
 * the module's does.
 */
public class NetworkRequest implements Parcelable {
    /** @hide */
    public final NetworkCapabilities networkCapabilities;

    /** @hide */
    public NetworkRequest(NetworkCapabilities capabilities) {
        networkCapabilities = new NetworkCapabilities(capabilities);
    }

    public NetworkRequest(NetworkRequest that) {
        this(that.networkCapabilities);
    }

    public boolean hasCapability(int capability) {
        return networkCapabilities.hasCapability(capability);
    }

    public boolean hasTransport(int transportType) {
        return networkCapabilities.hasTransport(transportType);
    }

    public int[] getCapabilities() {
        return networkCapabilities.getCapabilities();
    }

    public int[] getTransportTypes() {
        return networkCapabilities.getTransportTypes();
    }

    public NetworkSpecifier getNetworkSpecifier() {
        return networkCapabilities.getNetworkSpecifier();
    }

    public boolean canBeSatisfiedBy(NetworkCapabilities nc) {
        return networkCapabilities.satisfiedByNetworkCapabilities(nc);
    }

    @Override
    public String toString() {
        return "NetworkRequest [ " + networkCapabilities + " ]";
    }

    @Override
    public boolean equals(Object o) {
        return o instanceof NetworkRequest
                && networkCapabilities.equals(((NetworkRequest) o).networkCapabilities);
    }

    @Override
    public int hashCode() {
        return Objects.hash(networkCapabilities);
    }

    @Override
    public int describeContents() {
        return 0;
    }

    @Override
    public void writeToParcel(Parcel dest, int flags) {
        networkCapabilities.writeToParcel(dest, flags);
    }

    public static final Creator<NetworkRequest> CREATOR = new Creator<NetworkRequest>() {
        @Override
        public NetworkRequest createFromParcel(Parcel in) {
            return new NetworkRequest(NetworkCapabilities.CREATOR.createFromParcel(in));
        }

        @Override
        public NetworkRequest[] newArray(int size) {
            return new NetworkRequest[size];
        }
    };

    /** As NetworkRequest.Builder. */
    public static class Builder {
        private final NetworkCapabilities mCapabilities;

        public Builder() {
            mCapabilities = new NetworkCapabilities();
        }

        public Builder(NetworkRequest request) {
            mCapabilities = new NetworkCapabilities(request.networkCapabilities);
        }

        public NetworkRequest build() {
            return new NetworkRequest(mCapabilities);
        }

        public Builder addCapability(int capability) {
            mCapabilities.addCapability(capability);
            return this;
        }

        public Builder removeCapability(int capability) {
            mCapabilities.removeCapability(capability);
            return this;
        }

        public Builder clearCapabilities() {
            for (int c : mCapabilities.getCapabilities()) mCapabilities.removeCapability(c);
            return this;
        }

        public Builder addTransportType(int transportType) {
            mCapabilities.addTransportType(transportType);
            return this;
        }

        public Builder removeTransportType(int transportType) {
            mCapabilities.removeTransportType(transportType);
            return this;
        }

        public Builder setNetworkSpecifier(String networkSpecifier) {
            return setNetworkSpecifier(networkSpecifier == null ? null
                    : new StringNetworkSpecifier(networkSpecifier));
        }

        public Builder setNetworkSpecifier(NetworkSpecifier networkSpecifier) {
            mCapabilities.setNetworkSpecifier(networkSpecifier);
            return this;
        }

        public Builder setSignalStrength(int signalStrength) {
            mCapabilities.setSignalStrength(signalStrength);
            return this;
        }

        public Builder setIncludeOtherUidNetworks(boolean include) {
            return this;
        }

        public Builder setSubscriptionIds(java.util.Set<Integer> subIds) {
            return this;
        }

        /** @hide */
        public Builder setCapabilities(NetworkCapabilities nc) {
            mCapabilities.set(nc);
            return this;
        }
    }
}
