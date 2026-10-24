/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.net;

/**
 * A placeholder for the Connectivity module's NetworkRequest, for
 * {@link ConnectivityManager}'s signatures: requests are built and never satisfied.
 */
public class NetworkRequest {
    private final NetworkCapabilities mCapabilities;

    NetworkRequest(NetworkCapabilities capabilities) {
        mCapabilities = capabilities;
    }

    public boolean hasCapability(int capability) {
        return mCapabilities.hasCapability(capability);
    }

    public boolean hasTransport(int transportType) {
        return mCapabilities.hasTransport(transportType);
    }

    public int[] getCapabilities() {
        return mCapabilities.getCapabilities();
    }

    public int[] getTransportTypes() {
        return mCapabilities.getTransportTypes();
    }

    /** As NetworkRequest.Builder: INTERNET, NOT_RESTRICTED, TRUSTED and NOT_VPN by default. */
    public static class Builder {
        private final NetworkCapabilities mCapabilities = new NetworkCapabilities()
                .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                .addCapability(NetworkCapabilities.NET_CAPABILITY_NOT_RESTRICTED)
                .addCapability(NetworkCapabilities.NET_CAPABILITY_TRUSTED)
                .addCapability(NetworkCapabilities.NET_CAPABILITY_NOT_VPN);

        public Builder() {
        }

        public Builder(NetworkRequest request) {
            clearCapabilities();
            for (int c : request.getCapabilities()) mCapabilities.addCapability(c);
            for (int t : request.getTransportTypes()) mCapabilities.addTransportType(t);
        }

        public NetworkRequest build() {
            return new NetworkRequest(new NetworkCapabilities(mCapabilities));
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

        public Builder setIncludeOtherUidNetworks(boolean include) {
            return this;
        }
    }
}
