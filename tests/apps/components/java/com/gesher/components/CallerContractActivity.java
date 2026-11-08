/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.components;

import android.app.Activity;
import android.app.ActivityOptions;
import android.app.ComponentCaller;
import android.app.PendingIntent;
import android.content.ClipData;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Bundle;
import android.os.Process;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;

/** Actual ActivityThread/result delivery and a real private-provider descriptor. */
public final class CallerContractActivity extends Activity {
    private static final int REQUEST = 731;
    private static final Uri ITEM = Uri.parse("content://com.gesher.peer.caller/item?version=1#one");
    private boolean started;
    private int checks;

    private void check(boolean okay, String reason) {
        if (!okay) throw new AssertionError("ComponentCallerLifecycleTest: " + reason);
        checks++;
    }

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        if (state != null) throw new AssertionError("Unexpected source Activity recreation");
    }

    @Override protected void onResume() {
        super.onResume();
        if (started) return;
        started = true;
        try { getCurrentCaller(); throw new AssertionError("Current caller available outside callback"); }
        catch (IllegalStateException expected) { checks++; }
        Intent intent = new Intent().setClassName("com.gesher.peer", "com.gesher.peer.CallerContractActivity")
                .setData(ITEM);
        ArrayList<Uri> streams = new ArrayList<>();
        streams.add(Uri.parse("content://com.gesher.peer.caller/stream"));
        intent.putParcelableArrayListExtra(Intent.EXTRA_STREAM, streams);
        ClipData clip = ClipData.newRawUri("caller URI", Uri.parse("content://com.gesher.peer.caller/clip"));
        clip.addItem(new ClipData.Item(new Intent().setData(Uri.parse("content://com.gesher.peer.caller/nested"))));
        intent.setClipData(clip);
        // The other package is the holder/receiver. Its provider ownership must
        // never replace this creator's authority or identity when it sends them.
        Intent delegated = new Intent().setClassName("com.gesher.peer", "com.gesher.peer.CallerContractActivity")
                .setData(ITEM).addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        intent.putExtra("creatorShared", PendingIntent.getActivity(this, 734,
                new Intent(delegated).putExtra("phase", "creatorShared"),
                PendingIntent.FLAG_CANCEL_CURRENT | PendingIntent.FLAG_IMMUTABLE,
                ActivityOptions.makeBasic().setShareIdentityEnabled(true).toBundle()));
        intent.putExtra("senderShared", PendingIntent.getActivity(this, 735,
                new Intent(delegated).putExtra("phase", "senderShared"),
                PendingIntent.FLAG_CANCEL_CURRENT | PendingIntent.FLAG_IMMUTABLE));
        startActivityForResult(intent, REQUEST, ActivityOptions.makeBasic().setShareIdentityEnabled(true).toBundle());
    }

    @Override public void onActivityResult(int request, int result, Intent data, ComponentCaller caller) {
        super.onActivityResult(request, result, data, caller);
        check(request == REQUEST && result == RESULT_OK && data != null, "Peer lifecycle did not finish successfully");
        check(caller.equals(getCurrentCaller()), "Result callback/current caller mismatch");
        check(caller.getUid() == Process.INVALID_UID && caller.getPackage() == null,
                "Result exposed identity without sharing");
        check(ITEM.equals(data.getData()), "Result URI changed");
        check(caller.checkContentUriPermission(ITEM, Intent.FLAG_GRANT_READ_URI_PERMISSION)
                == PackageManager.PERMISSION_GRANTED, "Provider owner's historical read access missing");
        try {
            caller.checkContentUriPermission(ITEM, Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
            throw new AssertionError("Read-only recipient queried write without its own permission");
        } catch (SecurityException expected) { checks++; }
        try {
            caller.checkContentUriPermission(Uri.parse("content://com.gesher.peer.caller/neighbor"),
                    Intent.FLAG_GRANT_READ_URI_PERMISSION);
            throw new AssertionError("Neighbor URI inherited a grant");
        } catch (SecurityException expected) { checks++; }
        try (InputStream input = getContentResolver().openInputStream(ITEM)) {
            check(input != null, "Provider returned no stream");
            byte[] bytes = new byte[128];
            int count = input.read(bytes);
            check(count > 0 && "component-caller-real-provider".equals(new String(bytes, 0, count,
                    StandardCharsets.UTF_8)), "Real provider bytes did not cross Binder");
        } catch (java.io.IOException failure) { throw new AssertionError("Real provider I/O failed", failure); }
        int peerChecks = data.getIntExtra("peerChecks", 0);
        check(peerChecks >= 25, "Peer did not finish all lifecycle phases");
        Report.line("ComponentCallerLifecycleTest all passed: " + checks + " source checks, "
                + peerChecks + " peer checks; real initial/newIntent/result/recreate/PendingIntent/provider I/O");
        finish();
    }
}
