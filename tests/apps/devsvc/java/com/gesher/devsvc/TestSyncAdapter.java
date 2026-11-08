/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.devsvc;

import android.accounts.Account;
import android.app.Service;
import android.content.AbstractThreadedSyncAdapter;
import android.content.ContentProviderClient;
import android.content.Intent;
import android.content.SyncResult;
import android.os.Bundle;
import android.os.IBinder;

/** Local no-network sync fixture for account visibility/content-notification regressions. */
public final class TestSyncAdapter extends Service {
    private AbstractThreadedSyncAdapter adapter;
    @Override public void onCreate() {
        super.onCreate();
        adapter = new AbstractThreadedSyncAdapter(this, true) {
            @Override public void onPerformSync(Account account, Bundle extras, String authority,
                    ContentProviderClient provider, SyncResult result) {
                Report.line("sync: local fixture completed");
            }
        };
    }
    @Override public IBinder onBind(Intent intent) { return adapter.getSyncAdapterBinder(); }
}
