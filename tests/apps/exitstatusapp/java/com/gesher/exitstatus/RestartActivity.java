/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.exitstatus;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Process;
import android.widget.TextView;

public final class RestartActivity extends Activity {
    private static final String REPLACEMENT = "replacement";

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        TextView text = new TextView(this);
        text.setText("Restart after process exit");
        setContentView(text);
    }

    @Override public void onResume() {
        super.onResume();
        Handler main = new Handler(getMainLooper());
        if (getIntent().getBooleanExtra(REPLACEMENT, false)) {
            System.out.println("Exec4 exit app: replacement resumed pid=" + Process.myPid());
            main.postDelayed(this::finish, 500);
            return;
        }
        main.postDelayed(() -> {
            Intent restart = new Intent(Intent.ACTION_MAIN)
                    .addCategory(Intent.CATEGORY_LAUNCHER)
                    .setClass(this, RestartActivity.class)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK)
                    .putExtra(REPLACEMENT, true);
            startActivity(restart);
            System.out.println("Exec4 exit app: restart requested before exit pid=" + Process.myPid());
            // Exit from this same callback, before the main thread can acknowledge pause.
            System.exit(0);
        }, 300);
    }
}
