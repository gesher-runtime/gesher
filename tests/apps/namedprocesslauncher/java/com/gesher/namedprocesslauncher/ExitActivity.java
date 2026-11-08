/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.namedprocesslauncher;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Process;

public abstract class ExitActivity extends Activity {
    protected abstract int code();
    @Override public void onCreate(Bundle state){
        super.onCreate(state);
        int code=code();
        System.out.println("named-launch: worker-ready pid="+Process.myPid()+" code="+code+" process="+android.app.Application.getProcessName());
        // This starts a NEWER default-process generation after this named activity.
        startService(new Intent(this,SiblingService.class));
        new Handler().postDelayed(()->{
            System.out.println("named-launch: worker-exit pid="+Process.myPid()+" code="+code);
            System.out.flush();
            System.exit(code);
        },1200);
    }
}
