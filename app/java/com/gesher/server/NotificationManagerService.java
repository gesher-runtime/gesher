/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.INotificationManager;
import android.app.Notification;
import android.app.NotificationChannel;
import android.content.pm.ParceledListSlice;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * {@code notification}: NotificationManagerService, as a stand-in. It keeps the channels an
 * app creates and the notifications it posts (and those of its foreground services,
 * {@link ActiveServices}), logs them, and shows nothing: there is no status bar.
 */
public final class NotificationManagerService extends INotificationManager.Default {
    // Guarded by this. Keys: package/channel id, package/tag/id.
    private final Map<String, NotificationChannel> mChannels = new LinkedHashMap<>();
    private final Map<String, Notification> mPosted = new LinkedHashMap<>();

    private static String key(String pkg, String tag, int id) {
        return pkg + "/" + tag + "/" + id;
    }

    /** Posts a notification (enqueueNotificationWithTag, or a foreground service's). */
    void post(String pkg, String tag, int id, Notification notification) {
        synchronized (this) {
            mPosted.put(key(pkg, tag, id), notification);
        }
        CharSequence title = notification != null && notification.extras != null
                ? notification.extras.getCharSequence(Notification.EXTRA_TITLE) : null;
        SystemServer.log("notification " + id + " posted for " + pkg + ": channel "
                + (notification != null ? notification.getChannelId() : null) + ", title \""
                + title + "\" (not shown: no status bar)");
    }

    void cancel(String pkg, String tag, int id) {
        boolean removed;
        synchronized (this) {
            removed = mPosted.remove(key(pkg, tag, id)) != null;
        }
        if (removed) SystemServer.log("notification " + id + " of " + pkg + " cancelled");
    }

    @Override
    public void enqueueNotificationWithTag(String pkg, String opPkg, String tag, int id,
            Notification notification, int userId) {
        post(pkg, tag, id, notification);
    }

    @Override
    public void cancelNotificationWithTag(String pkg, String opPkg, String tag, int id,
            int userId) {
        cancel(pkg, tag, id);
    }

    @Override
    public void cancelAllNotifications(String pkg, int userId) {
        synchronized (this) {
            mPosted.keySet().removeIf(k -> k.startsWith(pkg + "/"));
        }
    }

    @Override
    public boolean areNotificationsEnabled(String pkg) {
        return true;
    }

    @Override
    public boolean areNotificationsEnabledForPackage(String pkg, int uid) {
        return true;
    }


    @Override
    public void createNotificationChannels(String pkg, ParceledListSlice channelsList) {
        List<?> channels = channelsList.getList();
        synchronized (this) {
            for (Object o : channels) {
                NotificationChannel c = (NotificationChannel) o;
                mChannels.put(pkg + "/" + c.getId(), c);
            }
        }
        for (Object o : channels) {
            SystemServer.log("notification channel " + ((NotificationChannel) o).getId()
                    + " created for " + pkg);
        }
    }

    @Override
    public NotificationChannel getNotificationChannel(String callingPkg, int userId,
            String pkg, String channelId) {
        synchronized (this) {
            return mChannels.get(pkg + "/" + channelId);
        }
    }

    @Override
    public ParceledListSlice getNotificationChannels(String callingPkg, String targetPkg,
            int userId) {
        List<NotificationChannel> result = new ArrayList<>();
        synchronized (this) {
            for (Map.Entry<String, NotificationChannel> e : mChannels.entrySet()) {
                if (e.getKey().startsWith(targetPkg + "/")) result.add(e.getValue());
            }
        }
        return new ParceledListSlice<>(result);
    }

    @Override
    public void deleteNotificationChannel(String pkg, String channelId) {
        synchronized (this) {
            mChannels.remove(pkg + "/" + channelId);
        }
    }
}
