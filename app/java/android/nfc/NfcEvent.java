/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.nfc;

public final class NfcEvent {
    public final NfcAdapter nfcAdapter;
    public final int peerLlcpVersion;

    NfcEvent(NfcAdapter nfcAdapter, int peerLlcpVersion) {
        this.nfcAdapter = nfcAdapter;
        this.peerLlcpVersion = peerLlcpVersion;
    }
}
