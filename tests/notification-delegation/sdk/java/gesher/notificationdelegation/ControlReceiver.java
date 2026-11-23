/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.notificationdelegation;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.SystemClock;
import android.service.notification.StatusBarNotification;
import org.json.JSONArray;
import org.json.JSONObject;

/** SDK-only reference corpus. Exported only in the isolated fixture APKs. */
public final class ControlReceiver extends BroadcastReceiver {
    @Override public void onReceive(Context context, Intent request) {
        JSONObject result = new JSONObject();
        try {
            NotificationManager manager = context.getSystemService(NotificationManager.class);
            String command = request.getStringExtra("command");
            String target = request.getStringExtra("target");
            String channel = request.getStringExtra("channel");
            if (channel == null) channel = "delegation";
            int id = request.getIntExtra("id", 1);
            String tag = request.getStringExtra("tag");
            result.put("package", context.getPackageName());
            result.put("uid", android.os.Process.myUid());
            result.put("command", command);
            switch (command) {
                case "channel":
                    manager.createNotificationChannel(new NotificationChannel(channel,
                            "Delegation contract", NotificationManager.IMPORTANCE_LOW));
                    result.put("channel", manager.getNotificationChannel(channel).getId());
                    break;
                case "set":
                    manager.setNotificationDelegate(target);
                    result.put("delegate", nullable(manager.getNotificationDelegate()));
                    break;
                case "get":
                    result.put("delegate", nullable(manager.getNotificationDelegate()));
                    break;
                case "can":
                    result.put("allowed", manager.canNotifyAsPackage(target));
                    break;
                case "notify":
                    Notification notification = new Notification.Builder(context, channel)
                            .setSmallIcon(android.R.drawable.ic_dialog_info)
                            .setContentTitle("Isolated notification delegation fixture")
                            .setContentText(context.getPackageName() + " / " + id).build();
                    if (target == null) manager.notify(tag, id, notification);
                    else manager.notifyAsPackage(target, tag, id, notification);
                    break;
                case "cancel":
                    if (target == null) manager.cancel(tag, id);
                    else manager.cancelAsPackage(target, tag, id);
                    break;
                case "clear":
                    manager.cancelAll();
                    break;
                case "active":
                    observeActive(manager, request, result);
                    break;
                case "channels":
                    Context selected = target == null ? context : context.createPackageContext(target, 0);
                    NotificationManager selectedManager = selected.getSystemService(NotificationManager.class);
                    NotificationChannel value = selectedManager.getNotificationChannel(channel);
                    result.put("channel", value == null ? JSONObject.NULL : value.getId());
                    result.put("count", selectedManager.getNotificationChannels().size());
                    break;
                default: throw new IllegalArgumentException("Unknown fixture command " + command);
            }
            result.put("ok", true);
        } catch (Throwable failure) {
            try {
                result.put("ok", false);
                result.put("exception", failure.getClass().getName());
                result.put("message", failure.getMessage());
            } catch (org.json.JSONException impossible) { throw new AssertionError(impossible); }
        }
        setResultCode(0);
        setResultData("F32_DELEGATION:" + result.toString());
        android.util.Log.i("F32_DELEGATION", result.toString());
    }

    private static Object nullable(Object value) { return value == null ? JSONObject.NULL : value; }

    private static JSONArray active(NotificationManager manager) throws org.json.JSONException {
        JSONArray entries = new JSONArray();
        for (StatusBarNotification value : manager.getActiveNotifications()) {
            JSONObject entry = new JSONObject();
            entry.put("owner", value.getPackageName());
            entry.put("opPkg", value.getOpPkg());
            entry.put("uid", value.getUid());
            entry.put("id", value.getId());
            entry.put("tag", nullable(value.getTag()));
            entries.put(entry);
        }
        return entries;
    }

    /** Retain every public-API sample; an absence window is evidence, not a proof of delivery. */
    private static void observeActive(NotificationManager manager, Intent request, JSONObject result)
            throws org.json.JSONException {
        if (!request.hasExtra("awaitId")) {
            result.put("active", active(manager));
            return;
        }
        int wantedId = request.getIntExtra("awaitId", -1);
        boolean wantedPresent = request.getBooleanExtra("awaitPresent", true);
        long timeout = 6500, stableWindow = wantedPresent ? 0 : 2500;
        long start = SystemClock.elapsedRealtime(), matchingSince = -1;
        boolean matched = false;
        JSONArray samples = new JSONArray(), values;
        do {
            values = active(manager);
            long elapsed = SystemClock.elapsedRealtime() - start;
            boolean present = false;
            for (int i = 0; i < values.length(); i++) {
                JSONObject value = values.getJSONObject(i);
                if (value.getInt("id") == wantedId) present = true;
            }
            samples.put(new JSONObject().put("elapsedMs", elapsed).put("active", values));
            if (present == wantedPresent) {
                if (matchingSince < 0) matchingSince = elapsed;
                if (elapsed - matchingSince >= stableWindow) { matched = true; break; }
            } else matchingSince = -1;
            if (elapsed >= timeout) break;
            SystemClock.sleep(100);
        } while (true);
        result.put("active", values);
        result.put("samples", samples);
        result.put("awaitId", wantedId);
        result.put("awaitPresent", wantedPresent);
        result.put("predicateObserved", matched);
        result.put("stableWindowMs", stableWindow);
        result.put("timeoutMs", timeout);
        result.put("elapsedMs", SystemClock.elapsedRealtime() - start);
        result.put("observation", wantedPresent ? "observed-presence" : "bounded-absence-window");
    }
}
