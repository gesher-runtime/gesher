/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.nfc;

import android.content.Context;

public final class NfcManager {
    public NfcManager(Context context) {}

    public NfcAdapter getDefaultAdapter() {
        return NfcAdapter.getDefaultAdapter();
    }
}
