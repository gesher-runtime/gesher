/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.orderedbroadcast.source;

import android.app.Activity;
import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.Bundle;
import android.os.Handler;
import java.lang.ref.WeakReference;

/** Headless sender; final receiver is retained only by the real broadcast machinery. */
public final class SourceActivity extends Activity {
    private static final String PERMISSION = "android.permission.ACCESS_NETWORK_STATE";
    private final Handler handler = new Handler();
    private boolean sent, reported, gcDone;
    private int finalCalls;
    private WeakReference<BroadcastReceiver> finalReference;
    private final BroadcastReceiver ready = new BroadcastReceiver() {
        @Override public void onReceive(Context context, Intent intent) {
            if (sent) return;
            sent = true;
            unregisterReceiver(this);
            sendEvent();
        }
    };
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        registerReceiver(ready, new IntentFilter("com.gesher.orderedbroadcast.READY"),
                PERMISSION, handler, Context.RECEIVER_EXPORTED);
        ComponentName started = startService(new Intent().setComponent(new ComponentName(
                "com.gesher.orderedbroadcast.target", "com.gesher.orderedbroadcast.target.TargetService")));
        if (started == null) { finishResult(false, "service-start"); return; }
        handler.postDelayed(() -> {
            if (!reported) finishResult(false, sent ? "timeout-final-callback" : "timeout-ready");
        }, 12000);
    }
    private void sendEvent() {
        BroadcastReceiver receiver = new BroadcastReceiver() {
            @Override public void onReceive(Context context, Intent intent) {
                ++finalCalls;
                Bundle result = getResultExtras(false);
                boolean valid = finalCalls == 1 && !isOrderedBroadcast() && gcDone
                        && getResultCode() == 303 && "manifest".equals(getResultData()) && result != null
                        && "manifest".equals(result.getString("stage"))
                        && "dynamic,manifest".equals(result.getString("trace"))
                        && result.getInt("dynamicPid",0) > 0 && result.getInt("manifestPid",0) > 0
                        && result.getInt("dynamicPid") != result.getInt("manifestPid");
                System.out.println("ordered: final received count=" + finalCalls + " orderedHint=" + isOrderedBroadcast()
                        + " code=" + getResultCode() + " propagated=" + valid + " gc=" + gcDone);
                if (!valid) { finishResult(false, "final-state"); return; }
                handler.postDelayed(() -> finishResult(finalCalls == 1, "final-once-after-gc"), 500);
            }
        };
        finalReference = new WeakReference<>(receiver);
        Bundle initial = new Bundle();
        initial.putString("stage", "initial"); initial.putString("trace", "");
        sendOrderedBroadcast(new Intent("com.gesher.orderedbroadcast.EVENT")
                .setPackage("com.gesher.orderedbroadcast.target"), PERMISSION,
                receiver, handler, 101, "initial", initial);
        receiver = null;
        // The call has returned and no local receiver reference survives these full GCs.
        handler.postDelayed(() -> {
            for (int i=0;i<4;i++) { System.gc(); System.runFinalization(); }
            gcDone = true;
            System.out.println("ordered: gc-complete receiverRetained=" + (finalReference.get()!=null));
        }, 30);
        System.out.println("ordered: event submitted receiverPermission=true");
    }
    private void finishResult(boolean pass, String reason) {
        if (reported) return;
        reported = true;
        System.out.println("ordered: " + (pass ? "PASS" : "FAIL") + " reason=" + reason + " finalCount=" + finalCalls);
        finish();
    }
}
