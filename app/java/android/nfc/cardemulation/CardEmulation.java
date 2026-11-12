/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.nfc.cardemulation;

import android.app.Activity;
import android.content.ComponentName;
import android.nfc.NfcAdapter;

import java.util.Collections;
import java.util.List;

public final class CardEmulation {
    public static final String ACTION_CHANGE_DEFAULT = "android.nfc.cardemulation.action.ACTION_CHANGE_DEFAULT";
    public static final String CATEGORY_PAYMENT = "payment";
    public static final String CATEGORY_OTHER = "other";
    public static final String EXTRA_CATEGORY = "category";
    public static final String EXTRA_SERVICE_COMPONENT = "component";

    public static final int SELECTION_MODE_PREFER_DEFAULT = 0;
    public static final int SELECTION_MODE_ALWAYS_ASK = 1;
    public static final int SELECTION_MODE_ASK_IF_CONFLICT = 2;

    private static final CardEmulation sInstance = new CardEmulation();

    private CardEmulation() {}

    public static synchronized CardEmulation getInstance(NfcAdapter adapter) {
        if (adapter == null) throw new NullPointerException("NfcAdapter is null");
        throw new UnsupportedOperationException("NFC card emulation is unavailable");
    }

    public boolean isDefaultServiceForCategory(ComponentName service, String category) {
        return false;
    }

    public boolean registerAidsForService(ComponentName service, String category, List<String> aids) {
        return false;
    }

    public List<String> getAidsForService(ComponentName service, String category) {
        return Collections.emptyList();
    }

    public boolean removeAidsForService(ComponentName service, String category) {
        return false;
    }

    public boolean setPreferredService(Activity activity, ComponentName service) {
        return false;
    }

    public boolean unsetPreferredService(Activity activity) {
        return false;
    }

    public boolean supportsAidPrefixRegistration() {
        return false;
    }

    public int getSelectionModeForCategory(String category) {
        return SELECTION_MODE_PREFER_DEFAULT;
    }
}
