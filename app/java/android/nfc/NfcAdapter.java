/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.nfc;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.Context;
import android.content.IntentFilter;
import android.os.Bundle;

/**
 * Compatibility stub for NfcAdapter on platforms without NFC hardware.
 */
public final class NfcAdapter {
    public static final String ACTION_TECH_DISCOVERED = "android.nfc.action.TECH_DISCOVERED";
    public static final String ACTION_TAG_DISCOVERED = "android.nfc.action.TAG_DISCOVERED";
    public static final String ACTION_NDEF_DISCOVERED = "android.nfc.action.NDEF_DISCOVERED";
    public static final String ACTION_ADAPTER_STATE_CHANGED = "android.nfc.action.ADAPTER_STATE_CHANGED";
    public static final String EXTRA_TAG = "android.nfc.extra.TAG";
    public static final String EXTRA_ID = "android.nfc.extra.ID";
    public static final String EXTRA_NDEF_MESSAGES = "android.nfc.extra.NDEF_MESSAGES";
    public static final String EXTRA_ADAPTER_STATE = "android.nfc.extra.ADAPTER_STATE";

    public static final int STATE_OFF = 1;
    public static final int STATE_TURNING_ON = 2;
    public static final int STATE_ON = 3;
    public static final int STATE_TURNING_OFF = 4;

    public static final int FLAG_READER_NFC_A = 0x1;
    public static final int FLAG_READER_NFC_B = 0x2;
    public static final int FLAG_READER_NFC_F = 0x4;
    public static final int FLAG_READER_NFC_V = 0x8;
    public static final int FLAG_READER_NFC_BARCODE = 0x10;
    public static final int FLAG_READER_SKIP_NDEF_CHECK = 0x80;
    public static final int FLAG_READER_NO_PLATFORM_SOUNDS = 0x100;

    public interface ReaderCallback {
        void onTagDiscovered(Tag tag);
    }

    public interface OnNdefPushCompleteCallback {
        void onNdefPushComplete(NfcEvent event);
    }

    public interface CreateNdefMessageCallback {
        NdefMessage createNdefMessage(NfcEvent event);
    }

    public interface CreateBeamUrisCallback {
        android.net.Uri[] createBeamUris(NfcEvent event);
    }

    private NfcAdapter() {}

    /**
     * Gesher does not expose NFC hardware. Android's no-hardware contract is
     * a null adapter, not a disabled but otherwise usable adapter instance.
     * Applications use this distinction to decide whether NFC is supported.
     */
    public static NfcAdapter getDefaultAdapter(Context context) {
        return null;
    }

    @Deprecated
    public static NfcAdapter getDefaultAdapter() {
        return null;
    }

    public boolean isEnabled() {
        return false;
    }

    public void enableForegroundDispatch(Activity activity, PendingIntent intent,
                                         IntentFilter[] filters, String[][] techLists) {}

    public void disableForegroundDispatch(Activity activity) {}

    public void enableReaderMode(Activity activity, ReaderCallback callback, int flags, Bundle extras) {}

    public void disableReaderMode(Activity activity) {}

    public void setNdefPushMessage(NdefMessage message, Activity activity, Activity... activities) {}

    public void setNdefPushMessageCallback(CreateNdefMessageCallback callback, Activity activity, Activity... activities) {}

    public void setOnNdefPushCompleteCallback(OnNdefPushCompleteCallback callback, Activity activity, Activity... activities) {}
}
