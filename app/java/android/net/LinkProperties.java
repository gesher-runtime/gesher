/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.net;

/**
 * A placeholder for the Connectivity module's LinkProperties, for
 * {@link ConnectivityManager}'s signatures: with no network there are no links.
 */
public final class LinkProperties {
    public LinkProperties() {
    }

    public String getInterfaceName() {
        return null;
    }

    public java.util.List<java.net.InetAddress> getDnsServers() {
        return java.util.Collections.emptyList();
    }

    public String getDomains() {
        return null;
    }

    public boolean isPrivateDnsActive() {
        return false;
    }
}
