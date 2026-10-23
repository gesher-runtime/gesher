/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.components;

import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.ServiceConnection;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.provider.Settings;

/**
 * The launcher activity. It runs the steps one after the other, each started when the
 * previous one reported its end ({@link #advance}): activities (a result, standard and
 * singleTop starts), a started service (stopService, then stopSelf), a bound service, a
 * foreground service, broadcasts (registered, then manifest), the app's content provider and
 * the settings provider. Then it finishes, and the system server ends the process.
 */
public class MainActivity extends ReportingActivity {
    static final String ACTION_PING = "com.gesher.components.PING";
    static final String ACTION_HELLO = "com.gesher.components.HELLO";
    private static final int REQUEST = 1;

    private static MainActivity sInstance;
    static int sTaskId = -1;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private int mStep;

    /** The current step ended: the next one runs (on the main thread). */
    static void advance() {
        MainActivity main = sInstance;
        main.mHandler.post(main::next);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        sInstance = this;
        sTaskId = getTaskId();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (mResumes == 1) {
            Report.line("step: activities");
            startActivityForResult(new Intent(this, SecondActivity.class).putExtra("level", 1),
                    REQUEST);
        } else if (mResumes == 2) {
            advance();
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        Report.line("MainActivity onActivityResult: request " + requestCode + ", result "
                + (resultCode == RESULT_OK ? "RESULT_OK" : String.valueOf(resultCode))
                + ", answer " + (data != null ? data.getIntExtra("answer", -1) : -1));
    }

    private final ServiceConnection mConnection = new ServiceConnection() {
        @Override
        public void onServiceConnected(ComponentName name, IBinder service) {
            boolean own = service instanceof LocalService.LocalBinder;
            Report.line("onServiceConnected " + name.getShortClassName() + ": "
                    + service.getClass().getSimpleName() + ", the service's own Binder: " + own
                    + (own ? ", add(2, 3) = "
                            + ((LocalService.LocalBinder) service).getService().add(2, 3) : ""));
            Report.line("unbindService");
            unbindService(this);
        }

        @Override
        public void onServiceDisconnected(ComponentName name) {
            Report.line("onServiceDisconnected " + name.getShortClassName());
        }
    };

    private final BroadcastReceiver mReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            Report.line("registered receiver onReceive " + intent.getAction() + ", n="
                    + intent.getIntExtra("n", 0));
            unregisterReceiver(this);
            Report.line("sendBroadcast to ManifestReceiver (explicit)");
            sendBroadcast(new Intent(ACTION_HELLO).setClass(MainActivity.this,
                    ManifestReceiver.class));
        }
    };

    private void next() {
        try {
            step(mStep++);
        } catch (Throwable t) {
            Report.line("FAILED in step " + (mStep - 1) + ": " + t);
            t.printStackTrace(System.out);
            finish();
        }
    }

    private void step(int step) {
        switch (step) {
            case 0:
                Report.line("step: started service, stopService");
                startService(new Intent(this, CounterService.class).putExtra("n", 1));
                startService(new Intent(this, CounterService.class).putExtra("n", 2));
                Report.line("stopService: " + stopService(new Intent(this, CounterService.class)));
                break;  // CounterService.onDestroy advances
            case 1:
                Report.line("step: started service, stopSelf");
                startService(new Intent(this, CounterService.class).putExtra("stopSelf", true));
                break;
            case 2:
                Report.line("step: bound service");
                Report.line("bindService: " + bindService(new Intent(this, LocalService.class),
                        mConnection, Context.BIND_AUTO_CREATE));
                break;  // LocalService.onDestroy advances
            case 3:
                Report.line("step: foreground service");
                startForegroundService(new Intent(this, ForegroundService.class));
                break;  // ForegroundService.onDestroy advances
            case 4:
                Report.line("step: broadcasts");
                registerReceiver(mReceiver, new IntentFilter(ACTION_PING),
                        Context.RECEIVER_NOT_EXPORTED);
                sendBroadcast(new Intent(ACTION_PING).setPackage(getPackageName())
                        .putExtra("n", 7));
                break;  // ManifestReceiver advances
            case 5:
                Report.line("step: content providers");
                providers();
                advance();
                break;
            default:
                Report.line("all steps ran");
                finish();
        }
    }

    private void providers() {
        ContentResolver cr = getContentResolver();
        for (String name : new String[] {"apple", "banana"}) {
            ContentValues values = new ContentValues();
            values.put(ItemsProvider.NAME, name);
            Uri uri = cr.insert(ItemsProvider.ITEMS, values);
            Report.line("insert " + name + ": " + uri);
        }
        StringBuilder names = new StringBuilder();
        int count;
        try (Cursor c = cr.query(ItemsProvider.ITEMS, null, null, null, null)) {
            count = c.getCount();
            while (c.moveToNext()) {
                if (names.length() > 0) names.append(", ");
                names.append(c.getString(c.getColumnIndexOrThrow(ItemsProvider.NAME)));
            }
        }
        Report.line("query " + ItemsProvider.ITEMS + ": " + count + " rows: " + names);

        Report.line("Settings.System font_scale=" + Settings.System.getFloat(cr,
                Settings.System.FONT_SCALE, -1f) + ", Settings.Secure android_id="
                + Settings.Secure.getString(cr, Settings.Secure.ANDROID_ID)
                + ", Settings.Global auto_time=" + Settings.Global.getInt(cr,
                        Settings.Global.AUTO_TIME, -1));
        Settings.System.putInt(cr, "gesher_test", 7);
        Report.line("Settings.System gesher_test after putInt: "
                + Settings.System.getInt(cr, "gesher_test", -1));
    }
}
