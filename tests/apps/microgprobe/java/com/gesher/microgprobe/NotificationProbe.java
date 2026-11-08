/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.microgprobe;

import android.app.Activity;
import android.app.INotificationManager;
import android.app.Notification;
import android.content.ComponentName;
import android.content.Context;
import android.os.Binder;
import android.os.Bundle;
import android.os.ServiceManager;
import android.service.notification.INotificationListener;
import android.util.Log;

/** Negative tests from an actually attached app UID, without displaying any notification. */
final class NotificationProbe {
    private interface Operation { void run() throws Exception; }
    private static void denied(String label, Operation operation) throws Exception {
        try { operation.run(); }
        catch (SecurityException expected) { Log.i("microgprobe", "PASS notification " + label); return; }
        throw new AssertionError("Unauthorized notification operation accepted: " + label);
    }
    static void run(Activity activity) throws Exception {
        INotificationManager manager=INotificationManager.Stub.asInterface(
                ServiceManager.getService(Context.NOTIFICATION_SERVICE));
        if (manager==null) throw new AssertionError("Missing notification service");
        String own=activity.getPackageName(), victim="com.gesher.devsvc";
        Notification notification=new Notification(); notification.extras=new Bundle();
        notification.extras.putString(Notification.EXTRA_TITLE,"non-personal rejected fixture");
        if (manager.areNotificationsEnabled(own)) throw new AssertionError("Ungrant POST reported enabled");
        manager.enqueueNotificationWithTag(own,own,"denied-probe",73419,notification,0);
        if (!manager.getAppActiveNotifications(own,0).getList().isEmpty()) {
            throw new AssertionError("POST denial exposed active notification content");
        }
        Log.i("microgprobe","PASS notification denied POST does not post content");
        denied("claimed owner enqueue",()->manager.enqueueNotificationWithTag(victim,own,null,73419,notification,0));
        denied("claimed owner cancel",()->manager.cancelNotificationWithTag(victim,own,null,73419,0));
        denied("claimed owner cancelAll",()->manager.cancelAllNotifications(victim,0));
        denied("claimed owner active query",()->manager.getAppActiveNotifications(victim,0));
        denied("claimed owner channels",()->manager.getNotificationChannels(own,victim,0));
        denied("claimed owner enabled query",()->manager.areNotificationsEnabled(victim));
        ComponentName ownComponent=new ComponentName(own,own+".NotAListener");
        ComponentName otherComponent=new ComponentName(victim,victim+".TestListener");
        INotificationListener fake=INotificationListener.Stub.asInterface(new Binder());
        denied("self special-access grant",()->manager.setNotificationListenerAccessGranted(ownComponent,true,true));
        denied("other special-access grant",()->manager.setNotificationListenerAccessGranted(otherComponent,true,true));
        denied("unapproved listener register",()->manager.registerListener(fake,ownComponent,0));
        denied("listener owner spoof",()->manager.registerListener(fake,otherComponent,0));
        denied("unregistered listener read",()->manager.getActiveNotificationsFromListener(fake,null,0));
        denied("null listener read",()->manager.getActiveNotificationsFromListener(null,null,0));
        denied("unregistered listener cancel",()->manager.cancelNotificationsFromListener(fake,null));
        denied("enabled listener enumeration",()->manager.getEnabledNotificationListeners(0));
    }
}
