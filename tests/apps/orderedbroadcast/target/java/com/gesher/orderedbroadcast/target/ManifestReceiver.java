/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.orderedbroadcast.target;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.os.Process;

/** Lower-priority receiver in a distinct app process verifies sequential result propagation. */
public final class ManifestReceiver extends BroadcastReceiver {
    @Override public void onReceive(Context context, Intent intent) {
        Bundle result = getResultExtras(false);
        boolean valid = isOrderedBroadcast() && getResultCode()==202 && "dynamic".equals(getResultData())
                && result!=null && "dynamic".equals(result.getString("stage"))
                && result.getInt("dynamicPid",0)>0 && result.getInt("dynamicPid")!=Process.myPid();
        System.out.println("ordered: manifest received ordered=" + isOrderedBroadcast() + " propagated=" + valid);
        Bundle next = result==null ? new Bundle() : new Bundle(result);
        next.putString("stage","manifest"); next.putString("trace", valid ? "dynamic,manifest" : "invalid");
        next.putInt("manifestPid",Process.myPid());
        setResult(303,"manifest",next);
    }
}
