/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.INotificationManager;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ParceledListSlice;
import android.content.pm.ResolveInfo;
import android.content.pm.ServiceInfo;
import android.os.Bundle;
import android.os.IBinder;
import android.os.RemoteException;
import android.os.UserHandle;
import android.service.notification.IDispatchCompletionListener;
import android.service.notification.INotificationListener;
import android.service.notification.NotificationListenerService;
import android.service.notification.NotificationRankingUpdate;
import android.service.notification.NotificationStats;
import android.service.notification.StatusBarNotification;

import com.gesher.app.shell.AppShell;

import java.io.File;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * {@code notification}: NotificationManagerService, with Windows toasts (devsvc track,
 * docs/services.md "Device services", ADR 0038). Each notification an app posts (and each
 * foreground service's, {@link ActiveServices}) is a toast of the app's AppUserModelID
 * ({@code Gesher.App.<package>}, as its windows and Start menu shortcut have; registered
 * under HKCU\Software\Classes\AppUserModelId with the app's label and icon so that it works
 * without a shortcut too): its title, text (or big text), sub text, the app's icon (or
 * the notification's large icon when it is a bitmap), progress, and its actions as
 * buttons. Tagged with the notification's id and tag, so posting it again replaces it
 * (quietly with FLAG_ONLY_ALERT_ONCE) and cancel, cancelAll and the app's uninstall take
 * it off the screen and the notification center. The channel's importance decides how it
 * shows: none, not at all; min and low, silently in the notification center; default,
 * a popup with sound; high, a popup of high priority. Clicking the toast (while the device
 * runs) sends its content intent (FLAG_AUTO_CANCEL cancels it), clicking a button its
 * action's intent.
 *
 * <p>Notification listeners, basics: the NotificationListenerServices of installed packages
 * (BIND_NOTIFICATION_LISTENER_SERVICE; every one is allowed) are bound while notifications
 * are posted, and get onListenerConnected, onNotificationPosted and onNotificationRemoved
 * with a ranking of the active notifications; getActiveNotifications answers them.
 */
public final class NotificationManagerService extends INotificationManager.Default
        implements WindowsHost.Listener {
    /** The toasts' group: one per app id, so the tag alone tells them apart. */
    private static final String TOAST_GROUP = "android";
    private static final String DEFAULT_CHANNEL = NotificationChannel.DEFAULT_CHANNEL_ID;

    /** A posted notification: NotificationRecord. */
    private static final class Record {
        final StatusBarNotification sbn;
        final NotificationChannel channel;

        Record(StatusBarNotification sbn, NotificationChannel channel) {
            this.sbn = sbn;
            this.channel = channel;
        }

        Notification n() {
            return sbn.getNotification();
        }
    }

    /** A notification listener: ManagedServiceInfo. */
    private final class Listener {
        final ComponentName component;  // null for registerListener's
        INotificationListener listener;
        android.app.IServiceConnection connection;

        Listener(ComponentName component) {
            this.component = component;
        }
    }

    // Guarded by this. Keys: package/channel id; package/tag/id.
    private final Map<String, NotificationChannel> mChannels = new LinkedHashMap<>();
    private final Map<String, Record> mPosted = new LinkedHashMap<>();
    private final List<Listener> mListeners = new ArrayList<>();
    private final Set<String> mRegisteredAppIds = new HashSet<>();
    private boolean mEventsStarted;
    private boolean mListenersBound;

    /** The service Services.create made last (for the job scheduler's job notifications). */
    static volatile NotificationManagerService sLast;

    public NotificationManagerService() {
        sLast = this;
    }

    private static String key(String pkg, String tag, int id) {
        return pkg + "/" + tag + "/" + id;
    }

    /** The toast's tag (Windows allows 64 characters): the id, and the tag if there is one. */
    static String toastTag(String tag, int id) {
        String t = tag == null ? String.valueOf(id) : id + "|" + tag;
        if (t.length() > 64) t = id + "|" + Integer.toHexString(tag.hashCode());
        return t;
    }

    private static ActivityManagerService am() {
        Services.Created c = Services.sLast;
        return c != null ? c.am : null;
    }

    private static PackageManagerService pm() {
        Services.Created c = Services.sLast;
        return c != null ? c.pm : null;
    }

    // --- posting ------------------------------------------------------------------------------

    /** Posts a notification (enqueueNotificationWithTag, or a foreground service's). */
    void post(String pkg, String tag, int id, Notification notification) {
        if (notification == null) return;
        NotificationChannel channel = channelFor(pkg, notification.getChannelId());
        StatusBarNotification sbn = new StatusBarNotification(pkg, pkg, id, tag,
                android.os.Process.myUid(), 0, notification, UserHandle.SYSTEM, null,
                System.currentTimeMillis());
        Record r = new Record(sbn, channel);
        boolean update;
        synchronized (this) {
            update = mPosted.put(key(pkg, tag, id), r) != null;
        }
        CharSequence title = notification.extras != null
                ? notification.extras.getCharSequence(Notification.EXTRA_TITLE) : null;
        int importance = channel.getImportance();
        String shown;
        if (importance == NotificationManager.IMPORTANCE_NONE) {
            shown = "not shown: its channel is blocked";
        } else {
            int hr = showToast(r, update);
            shown = hr == 0 ? "a toast of " + AppShell.appUserModelId(pkg)
                    : String.format("no toast (0x%08x)", hr);
        }
        SystemServer.log("notification " + id + (update ? " updated" : " posted") + " for "
                + pkg + ": channel " + channel.getId() + ", title \"" + title
                + "\" (importance " + importance + ", " + shown + ")");
        notifyListeners(r, true, 0);
    }

    void cancel(String pkg, String tag, int id) {
        cancel(pkg, tag, id, NotificationListenerService.REASON_APP_CANCEL);
    }

    private void cancel(String pkg, String tag, int id, int reason) {
        Record r;
        synchronized (this) {
            r = mPosted.remove(key(pkg, tag, id));
        }
        if (r == null) return;
        int hr = WindowsHost.removeToast(AppShell.appUserModelId(pkg), toastTag(tag, id),
                TOAST_GROUP);
        SystemServer.log("notification " + id + " of " + pkg + " cancelled"
                + (hr == 0 ? ", its toast removed" : String.format(" (toast: 0x%08x)", hr)));
        notifyListeners(r, false, reason);
    }

    private NotificationChannel channelFor(String pkg, String channelId) {
        synchronized (this) {
            NotificationChannel c = mChannels.get(pkg + "/" + channelId);
            if (c != null) return c;
        }
        // No such channel (Android drops the notification of an app targeting O or later;
        // here it is shown with the default importance, logged).
        return new NotificationChannel(channelId != null ? channelId : DEFAULT_CHANNEL,
                "Default", NotificationManager.IMPORTANCE_DEFAULT);
    }

    private int showToast(Record r, boolean update) {
        String pkg = r.sbn.getPackageName();
        String appId = AppShell.appUserModelId(pkg);
        startEvents();
        File icon = registerAppId(pkg, appId);
        Notification n = r.n();
        File large = NotificationIcons.largeIcon(pkg, r.sbn.getKey(), n.getLargeIcon());
        int importance = r.channel.getImportance();
        boolean quiet = importance == NotificationManager.IMPORTANCE_MIN
                || importance == NotificationManager.IMPORTANCE_LOW;
        boolean suppressPopup = quiet
                || (update && (n.flags & Notification.FLAG_ONLY_ALERT_ONCE) != 0);
        boolean high = importance >= NotificationManager.IMPORTANCE_HIGH;
        String xml = toastXml(r, large != null ? large : icon, quiet || update);
        return WindowsHost.showToast(appId, toastTag(r.sbn.getTag(), r.sbn.getId()),
                TOAST_GROUP, xml, suppressPopup, high);
    }

    /** Names the app id after the app (label and icon), once per device run. */
    private File registerAppId(String pkg, String appId) {
        PackageManagerService pm = pm();
        ApkPackage p = pm != null ? pm.get(pkg) : null;
        if (p == null) return null;
        NotificationIcons.AppPicture picture = NotificationIcons.app(p);
        synchronized (this) {
            if (!mRegisteredAppIds.add(appId)) return picture.png;
        }
        String iconPath = picture.png != null ? WindowsHost.fullPath(picture.png.getPath()) : "";
        int hr = WindowsHost.registerAppId(appId, picture.label, iconPath);
        if (hr != 0) SystemServer.log(String.format("notification: registering %s: 0x%08x", appId, hr));
        return picture.png;
    }

    /** The toast's XML (ToastGeneric): what the notification shows. */
    private static String toastXml(Record r, File image, boolean silent) {
        Notification n = r.n();
        Bundle e = n.extras != null ? n.extras : new Bundle();
        CharSequence title = e.getCharSequence(Notification.EXTRA_TITLE);
        CharSequence text = e.getCharSequence(Notification.EXTRA_BIG_TEXT);
        if (text == null) text = e.getCharSequence(Notification.EXTRA_TEXT);
        CharSequence[] lines = e.getCharSequenceArray(Notification.EXTRA_TEXT_LINES);
        if (text == null && lines != null && lines.length > 0) {
            text = String.join("\n", java.util.Arrays.asList(lines));
        }
        CharSequence sub = e.getCharSequence(Notification.EXTRA_SUB_TEXT);
        if (sub == null) sub = e.getCharSequence(Notification.EXTRA_INFO_TEXT);
        if (title == null && text == null && n.tickerText != null) title = n.tickerText;
        String key = r.sbn.getKey();
        StringBuilder x = new StringBuilder();
        x.append("<toast launch=\"").append(escape("n|" + key)).append('"');
        if ((n.flags & Notification.FLAG_ONGOING_EVENT) != 0) x.append(" duration=\"long\"");
        x.append("><visual><binding template=\"ToastGeneric\">");
        x.append("<text>").append(escape(title != null ? title : "")).append("</text>");
        if (text != null) x.append("<text>").append(escape(text)).append("</text>");
        if (sub != null) {
            x.append("<text placement=\"attribution\">").append(escape(sub)).append("</text>");
        }
        if (image != null) {
            x.append("<image placement=\"appLogoOverride\" src=\"")
                    .append(escape(fileUri(image))).append("\"/>");
        }
        int max = e.getInt(Notification.EXTRA_PROGRESS_MAX, 0);
        boolean indeterminate = e.getBoolean(Notification.EXTRA_PROGRESS_INDETERMINATE);
        if (max > 0 || indeterminate) {
            int progress = e.getInt(Notification.EXTRA_PROGRESS, 0);
            x.append("<progress status=\"").append(escape(sub != null ? sub : "")).append('"');
            if (indeterminate) {
                x.append(" value=\"indeterminate\"/>");
            } else {
                double v = Math.max(0, Math.min(1, progress / (double) max));
                x.append(" value=\"").append(String.format(java.util.Locale.ROOT, "%.3f", v))
                        .append("\" valueStringOverride=\"").append(Math.round(v * 100))
                        .append("%\"/>");
            }
        }
        x.append("</binding></visual>");
        if (n.actions != null && n.actions.length > 0) {
            x.append("<actions>");
            for (int i = 0; i < n.actions.length && i < 5; i++) {
                Notification.Action a = n.actions[i];
                if (a == null || a.title == null) continue;
                x.append("<action content=\"").append(escape(a.title)).append("\" arguments=\"")
                        .append(escape("a|" + i + "|" + key)).append("\"/>");
            }
            x.append("</actions>");
        }
        if (silent) x.append("<audio silent=\"true\"/>");
        x.append("</toast>");
        return x.toString();
    }

    private static String fileUri(File f) {
        return "file:///" + WindowsHost.fullPath(f.getPath()).replace('\\', '/');
    }

    private static String escape(CharSequence s) {
        StringBuilder b = new StringBuilder(s.length());
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            switch (c) {
                case '<': b.append("&lt;"); break;
                case '>': b.append("&gt;"); break;
                case '&': b.append("&amp;"); break;
                case '"': b.append("&quot;"); break;
                case '\'': b.append("&apos;"); break;
                default:
                    if (c >= 0x20 || c == '\n' || c == '\t') b.append(c);
            }
        }
        return b.toString();
    }

    // --- clicks ---------------------------------------------------------------------------------

    private void startEvents() {
        synchronized (this) {
            if (mEventsStarted) return;
            mEventsStarted = true;
        }
        WindowsHost.addListener(this);
    }

    @Override
    public void onWindowsEvents(int events) {
        if ((events & WindowsHost.EVENT_TOAST_ACTIVATED) == 0) return;
        String[] a;
        while ((a = WindowsHost.nextActivation()) != null) activated(a[3]);
    }

    /** A toast or one of its buttons was clicked: "n|key" or "a|index|key". */
    private void activated(String arguments) {
        String key;
        int action = -1;
        if (arguments.startsWith("n|")) {
            key = arguments.substring(2);
        } else if (arguments.startsWith("a|")) {
            int bar = arguments.indexOf('|', 2);
            if (bar < 0) return;
            try {
                action = Integer.parseInt(arguments.substring(2, bar));
            } catch (NumberFormatException e) {
                return;
            }
            key = arguments.substring(bar + 1);
        } else {
            return;
        }
        Record r = null;
        synchronized (this) {
            for (Record p : mPosted.values()) {
                if (p.sbn.getKey().equals(key)) r = p;
            }
        }
        if (r == null) {
            SystemServer.log("notification: a toast was clicked whose notification is gone");
            return;
        }
        Notification n = r.n();
        PendingIntent intent = action >= 0 && n.actions != null && action < n.actions.length
                ? n.actions[action].actionIntent : n.contentIntent;
        SystemServer.log("notification " + r.sbn.getId() + " of " + r.sbn.getPackageName()
                + ": its toast was clicked" + (action >= 0 ? " (action " + action + ")" : "")
                + (intent != null ? ", sending " + intent : ", nothing to send"));
        ActivityManagerService am = am();
        if (intent != null && am != null) {
            try {
                am.sendIntentSender(null, intent.getTarget(), null, 0, null, null, null, null,
                        null);
            } catch (Throwable t) {
                SystemServer.log("notification: sending " + intent + " failed: " + t);
            }
        }
        if (action < 0 && (n.flags & Notification.FLAG_AUTO_CANCEL) != 0) {
            cancel(r.sbn.getPackageName(), r.sbn.getTag(), r.sbn.getId(),
                    NotificationListenerService.REASON_CLICK);
        }
    }

    // --- listeners ------------------------------------------------------------------------------

    /** Binds the packages' notification listener services, once notifications are posted. */
    private void bindListeners() {
        synchronized (this) {
            if (mListenersBound) return;
            mListenersBound = true;
        }
        PackageManagerService pm = pm();
        ActivityManagerService am = am();
        if (pm == null || am == null) return;
        for (ResolveInfo ri : pm.services(new Intent(NotificationListenerService.SERVICE_INTERFACE),
                null, PackageManager.GET_META_DATA)) {
            ServiceInfo si = ri.serviceInfo;
            if (!android.Manifest.permission.BIND_NOTIFICATION_LISTENER_SERVICE.equals(
                    si.permission)) {
                continue;
            }
            bindListener(new ComponentName(si.packageName, si.name));
        }
    }

    private void bindListener(ComponentName component) {
        ActivityManagerService am = am();
        if (am == null) return;
        Listener l = new Listener(component);
        l.connection = new android.app.IServiceConnection.Stub() {
            @Override
            public void connected(ComponentName name, IBinder service,
                    android.app.IBinderSession session, boolean dead) {
                if (service == null) return;
                INotificationListener listener = INotificationListener.Stub.asInterface(service);
                synchronized (NotificationManagerService.this) {
                    l.listener = listener;
                }
                SystemServer.log("notification: listener " + component.flattenToShortString()
                        + " connected");
                try {
                    listener.onListenerConnected(rankingUpdate(), new NoCompletion(), 0);
                } catch (RemoteException e) {
                    SystemServer.log("notification: onListenerConnected: " + e);
                }
            }
        };
        synchronized (this) {
            mListeners.add(l);
        }
        Intent intent = new Intent(NotificationListenerService.SERVICE_INTERFACE)
                .setComponent(component);
        try {
            am.bindService(null, null, intent, null, l.connection, Context.BIND_AUTO_CREATE,
                    "android", 0);
            SystemServer.log("notification: binding listener " + component.flattenToShortString());
        } catch (Throwable t) {
            SystemServer.log("notification: binding " + component + " failed: " + t);
        }
    }

    /** Unbinds the listener services once no notification is posted. */
    private void maybeUnbindListeners() {
        List<Listener> unbind = new ArrayList<>();
        synchronized (this) {
            if (!mListenersBound || !mPosted.isEmpty()) return;
            mListenersBound = false;
            for (int i = mListeners.size() - 1; i >= 0; i--) {
                Listener l = mListeners.get(i);
                if (l.component != null) {
                    unbind.add(l);
                    mListeners.remove(i);
                }
            }
        }
        ActivityManagerService am = am();
        for (Listener l : unbind) {
            if (am != null) am.unbindService(l.connection);
        }
    }

    private static final class NoCompletion extends IDispatchCompletionListener.Stub {
        @Override
        public void notifyDispatchComplete(long dispatchToken) {
        }
    }

    private NotificationRankingUpdate rankingUpdate() {
        List<NotificationListenerService.Ranking> rankings = new ArrayList<>();
        synchronized (this) {
            int rank = 0;
            for (Record r : mPosted.values()) {
                NotificationListenerService.Ranking ranking = new NotificationListenerService.Ranking();
                ranking.populate(r.sbn.getKey(), rank++, true /* matchesInterruptionFilter */,
                        NotificationListenerService.Ranking.VISIBILITY_NO_OVERRIDE,
                        0 /* suppressedVisualEffects */, r.channel.getImportance(),
                        null /* explanation */, null /* overrideGroupKey */, r.channel,
                        null /* overridePeople */, null /* snoozeCriteria */,
                        true /* showBadge */, 0 /* userSentiment */, false /* hidden */,
                        0 /* lastAudiblyAlertedMs */, false /* noisy */,
                        null /* smartActions */, null /* smartReplies */,
                        false /* canBubble */, false /* isTextChanged */,
                        false /* isConversation */, null /* shortcutInfo */,
                        0 /* rankingAdjustment */, false /* isBubble */,
                        0 /* proposedImportance */, false /* sensitiveContent */,
                        null /* summarization */);
                rankings.add(ranking);
            }
        }
        return new NotificationRankingUpdate(
                rankings.toArray(new NotificationListenerService.Ranking[0]));
    }

    private void notifyListeners(Record r, boolean posted, int reason) {
        if (posted) bindListeners();
        List<INotificationListener> targets = new ArrayList<>();
        synchronized (this) {
            for (Listener l : mListeners) {
                if (l.listener != null) targets.add(l.listener);
            }
        }
        if (!targets.isEmpty()) {
            NotificationRankingUpdate update = rankingUpdate();
            for (INotificationListener l : targets) {
                try {
                    if (posted) {
                        l.onNotificationPostedFull(r.sbn, update, 0);
                    } else {
                        l.onNotificationRemovedFull(r.sbn, update, new NotificationStats(),
                                reason, 0);
                    }
                } catch (RemoteException e) {
                    SystemServer.log("notification: a listener is gone: " + e);
                }
            }
        }
        if (!posted) maybeUnbindListeners();
    }

    private synchronized List<StatusBarNotification> active(String pkg) {
        List<StatusBarNotification> result = new ArrayList<>();
        for (Record r : mPosted.values()) {
            if (pkg == null || pkg.equals(r.sbn.getPackageName())) result.add(r.sbn);
        }
        return result;
    }

    // --- INotificationManager -------------------------------------------------------------------

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
        List<Record> removed = new ArrayList<>();
        synchronized (this) {
            mPosted.entrySet().removeIf(e -> {
                if (!e.getKey().startsWith(pkg + "/")) return false;
                // Foreground services' notifications stay (as on Android).
                if ((e.getValue().n().flags & Notification.FLAG_FOREGROUND_SERVICE) != 0) {
                    return false;
                }
                removed.add(e.getValue());
                return true;
            });
        }
        String appId = AppShell.appUserModelId(pkg);
        for (Record r : removed) {
            WindowsHost.removeToast(appId, toastTag(r.sbn.getTag(), r.sbn.getId()), TOAST_GROUP);
        }
        SystemServer.log("notification: all of " + pkg + " cancelled (" + removed.size() + ")");
        for (Record r : removed) {
            notifyListeners(r, false, NotificationListenerService.REASON_APP_CANCEL_ALL);
        }
    }

    /** Removes every notification of the package (it is uninstalled or stopped for good). */
    void packageRemoved(String pkg) {
        synchronized (this) {
            mPosted.keySet().removeIf(k -> k.startsWith(pkg + "/"));
            mChannels.keySet().removeIf(k -> k.startsWith(pkg + "/"));
        }
        String appId = AppShell.appUserModelId(pkg);
        WindowsHost.clearToasts(appId);
        WindowsHost.unregisterAppId(appId);
        synchronized (this) {
            mRegisteredAppIds.remove(appId);
        }
    }

    @Override
    public ParceledListSlice getAppActiveNotifications(String callingPkg, int userId) {
        return new ParceledListSlice<>(active(callingPkg));
    }

    @Override
    public StatusBarNotification[] getActiveNotifications(String callingPkg) {
        return active(null).toArray(new StatusBarNotification[0]);
    }

    @Override
    public StatusBarNotification[] getActiveNotificationsWithAttribution(String callingPkg,
            String callingAttributionTag) {
        return getActiveNotifications(callingPkg);
    }

    @Override
    public ParceledListSlice getActiveNotificationsFromListener(INotificationListener token,
            String[] keys, int trim) {
        List<StatusBarNotification> all = active(null);
        if (keys != null) {
            List<String> wanted = java.util.Arrays.asList(keys);
            all.removeIf(sbn -> !wanted.contains(sbn.getKey()));
        }
        return new ParceledListSlice<>(all);
    }

    @Override
    public void registerListener(INotificationListener listener, ComponentName component,
            int userid) {
        Listener l = new Listener(null);
        l.listener = listener;
        synchronized (this) {
            mListeners.add(l);
        }
        try {
            listener.onListenerConnected(rankingUpdate(), new NoCompletion(), 0);
        } catch (RemoteException e) {
            SystemServer.log("notification: onListenerConnected: " + e);
        }
    }

    @Override
    public void unregisterListener(INotificationListener listener, int userid) {
        synchronized (this) {
            mListeners.removeIf(l -> l.listener != null
                    && l.listener.asBinder() == listener.asBinder());
        }
    }

    @Override
    public void requestBindListener(ComponentName component) {
        synchronized (this) {
            for (Listener l : mListeners) {
                if (component.equals(l.component)) return;
            }
        }
        bindListener(component);
    }

    @Override
    public void cancelNotificationFromListener(INotificationListener token, String pkg,
            String tag, int id) {
        cancel(pkg, tag, id, NotificationListenerService.REASON_CANCEL);
    }

    @Override
    public void cancelNotificationsFromListener(INotificationListener token, String[] keys) {
        List<Record> matched = new ArrayList<>();
        synchronized (this) {
            for (Record r : mPosted.values()) {
                if (keys == null || java.util.Arrays.asList(keys).contains(r.sbn.getKey())) {
                    matched.add(r);
                }
            }
        }
        for (Record r : matched) {
            cancel(r.sbn.getPackageName(), r.sbn.getTag(), r.sbn.getId(),
                    NotificationListenerService.REASON_CANCEL);
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
            NotificationChannel c = (NotificationChannel) o;
            SystemServer.log("notification channel " + c.getId() + " created for " + pkg
                    + " (importance " + c.getImportance() + ")");
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
