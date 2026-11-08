/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.windowpromotion.target;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/** Test-only, public synthetic component; the private host controls phase transition. */
public final class ControlReceiver extends BroadcastReceiver {
    @Override public void onReceive(Context context, Intent intent) {
        if (BridgeActivity.instance != null) {
            BridgeActivity.instance.revealContent();
            return;
        }
        if (KeepAliveService.instance == null) throw new AssertionError("Original service gone");
        KeepAliveService.instance.beginUnapprovedPhase();
    }
}
