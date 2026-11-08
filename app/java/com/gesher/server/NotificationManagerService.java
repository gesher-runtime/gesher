/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.INotificationManager;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationChannelGroup;
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
import android.os.Binder;
import android.os.IBinder;
import android.os.RemoteException;
import android.os.UserHandle;
import android.service.notification.IDispatchCompletionListener;
import android.service.notification.INotificationListener;
import android.service.notification.NotificationListenerService;
import android.service.notification.NotificationRankingUpdate;
import android.service.notification.NotificationStats;
import android.service.notification.StatusBarNotification;
import android.util.AtomicFile;
import android.util.Xml;

import com.android.internal.notification.NotificationChannelGroupsHelper;
import com.android.modules.utils.TypedXmlPullParser;
import com.android.modules.utils.TypedXmlSerializer;
import org.xmlpull.v1.XmlPullParser;

import com.gesher.app.shell.AppShell;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.Properties;

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
 * (BIND_NOTIFICATION_LISTENER_SERVICE; explicit special access in protected mode) are bound while notifications
 * are posted, and get onListenerConnected, onNotificationPosted and onNotificationRemoved
 * with a ranking of the active notifications; getActiveNotifications answers them.
 */
public final class NotificationManagerService extends INotificationManager.Default
        implements WindowsHost.Listener {
    /** The toasts' group: one per app id, so the tag alone tells them apart. */
    private static final String TOAST_GROUP = "android";
    private static final String DEFAULT_CHANNEL = NotificationChannel.DEFAULT_CHANNEL_ID;
    private static final int CHANNEL_LIMIT = 5000;
    private static final int GROUP_LIMIT = 6000;

    /** A posted notification: NotificationRecord. */
    private static final class Record {
        final StatusBarNotification sbn;
        final NotificationChannel channel;
        final int importance;

        Record(StatusBarNotification sbn, NotificationChannel channel, int importance) {
            this.sbn = sbn;
            this.channel = channel;
            this.importance = importance;
        }

        Notification n() {
            return sbn.getNotification();
        }
    }

    /** A notification listener: ManagedServiceInfo. */
    private final class Listener {
        final ComponentName component;
        final String identity;
        INotificationListener listener;
        android.app.IServiceConnection connection;

        Listener(ComponentName component) {
            this.component = component;
            this.identity = component == null ? null : codeIdentity(packageFor(component.getPackageName()));
        }
    }

    // Guarded by this. Published channel/group objects are never modified in place.
    // Deleted channels remain as tombstones, as in Android's PreferencesHelper.
    private final Map<String, NotificationChannel> mChannels = new LinkedHashMap<>();
    private final Map<String, NotificationChannelGroup> mChannelGroups = new LinkedHashMap<>();
    private final Set<String> mLoadedChannelPackages = new HashSet<>();
    private final Map<String, Record> mPosted = new LinkedHashMap<>();
    private final Object mPublicationLock = new Object();
    private JobSchedulerService mJobScheduler; // Guarded by mPublicationLock.
    private final ThreadLocal<List<Runnable>> mDeferredListenerChanges = new ThreadLocal<>();
    private Runnable mBeforePostPublication; // Server-local race fixture; never a Binder option.
    private final List<Listener> mListeners = new ArrayList<>();
    private final Set<String> mRegisteredAppIds = new HashSet<>();
    private boolean mEventsStarted;
    private boolean mListenersBound;
    private final PackageManagerService mPackageManager;
    private final ActivityManagerService mActivityManager;
    private final java.util.function.ToIntBiFunction<StatusBarNotification, Boolean> mTestToast;
    private final File mListenerFile = new File(Device.dataDir(), "system/notification-listeners");
    private final Properties mListenerGrants = new Properties();

    /** The service Services.create made last (for the job scheduler's job notifications). */
    static volatile NotificationManagerService sLast;

    public NotificationManagerService() {
        this(null, null, null);
    }

    /** Server-local injection for tests; never exposed through Binder. */
    NotificationManagerService(PackageManagerService pm, ActivityManagerService am,
            java.util.function.ToIntBiFunction<StatusBarNotification, Boolean> testToast) {
        mPackageManager = pm;
        mActivityManager = am;
        mTestToast = testToast;
        Properties loaded = Device.read(mListenerFile);
        if (loaded != null) mListenerGrants.putAll(loaded);
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

    private ActivityManagerService am() {
        if (mActivityManager != null) return mActivityManager;
        Services.Created c = Services.sLast;
        return c != null ? c.am : null;
    }

    private PackageManagerService pm() {
        if (mPackageManager != null) return mPackageManager;
        Services.Created c = Services.sLast;
        return c != null ? c.pm : null;
    }

    private boolean protectedMode() { return GmsSignatureSpoofing.isEnabled(); }

    private CallerIdentity callers() {
        ActivityManagerService am = am();
        return am != null ? am.callers() : new CallerIdentity(pm(), null);
    }

    private void user(int userId) {
        if (userId != 0 && !(userId == UserHandle.USER_ALL && callers().isSystem())) {
            throw new SecurityException("Only user 0 is supported");
        }
    }

    private void own(String pkg) {
        if (pkg == null) throw new SecurityException("Missing notification owner");
        callers().requireSystemOrPackage(pkg);
        if (packageFor(pkg) == null && !(callers().isSystem() && "android".equals(pkg))) {
            throw new SecurityException("Unknown notification owner");
        }
    }

    private void system() {
        if (!callers().isSystem()) {
            throw new SecurityException("Notification administration requires the host shell");
        }
    }

    private ApkPackage packageFor(String pkg) { return pm() == null ? null : pm().get(pkg); }

    private boolean mayPost(String pkg) {
        if (pm() != null && pm().isRemovalPending(pkg)) return false;
        if (!protectedMode()) return true;
        if ("android".equals(pkg)) return true; // enqueue ownership already restricts this virtual owner.
        PackageManagerService pm = pm();
        return pm != null && pm.permissions().checkPermission(pkg,
                android.Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED;
    }

    /** A verified signer survives safe updates; unsigned developer code is exact-code-bound. */
    static String codeIdentity(ApkPackage pkg) {
        if (pkg == null) return null;
        android.content.pm.Signature[] signers = pkg.verifiedSignatures();
        try {
            java.security.MessageDigest digest = java.security.MessageDigest.getInstance("SHA-256");
            if (signers != null) {
                for (android.content.pm.Signature signer : signers) {
                    byte[] bytes = signer.toByteArray();
                    digest.update(java.nio.ByteBuffer.allocate(4).putInt(bytes.length).array());
                    digest.update(bytes);
                }
            } else {
                List<File> files = new ArrayList<>();
                files.add(new File(pkg.apkPath));
                File parent = new File(pkg.apkPath).getParentFile();
                File[] splits = parent == null ? null : parent.listFiles(
                        f -> f.getName().startsWith("split_") && f.getName().endsWith(".apk"));
                if (splits != null) {
                    java.util.Arrays.sort(splits, java.util.Comparator.comparing(File::getName));
                    java.util.Collections.addAll(files, splits);
                }
                files.sort(java.util.Comparator.comparing(File::getName));
                byte[] buffer = new byte[65536];
                for (File file : files) {
                    byte[] name = file.getName().getBytes(java.nio.charset.StandardCharsets.UTF_8);
                    digest.update(java.nio.ByteBuffer.allocate(4).putInt(name.length).array());
                    digest.update(name);
                    digest.update(java.nio.ByteBuffer.allocate(8).putLong(file.length()).array());
                    try (java.io.InputStream input = new java.io.FileInputStream(file)) {
                        for (int n; (n = input.read(buffer)) >= 0;) if (n > 0) digest.update(buffer, 0, n);
                    }
                }
            }
            StringBuilder hex = new StringBuilder();
            for (byte b : digest.digest()) hex.append(String.format(java.util.Locale.ROOT, "%02x", b & 255));
            return (signers == null ? "apk:" : "cert:") + hex;
        } catch (Exception | LinkageError e) { return null; }
    }

    private ServiceInfo listenerService(ComponentName component) {
        if (component == null || pm() == null) return null;
        for (ResolveInfo info : pm().services(new Intent(NotificationListenerService.SERVICE_INTERFACE)
                .setComponent(component), null, PackageManager.GET_META_DATA)) {
            ServiceInfo service = info.serviceInfo;
            if (service != null && component.getPackageName().equals(service.packageName)
                    && component.getClassName().equals(service.name)
                    && android.Manifest.permission.BIND_NOTIFICATION_LISTENER_SERVICE.equals(service.permission)) {
                return service;
            }
        }
        return null;
    }

    private synchronized boolean listenerAllowed(ComponentName component) {
        if (!protectedMode()) return true;
        if (component == null || listenerService(component) == null) return false;
        String identity = codeIdentity(packageFor(component.getPackageName()));
        return identity != null && identity.equals(mListenerGrants.getProperty(component.flattenToString()));
    }

    private Listener listenerAccess(INotificationListener token) {
        if (!protectedMode()) return null;
        if (token == null) throw new SecurityException("Missing listener token");
        synchronized (this) {
            for (Listener listener : mListeners) {
                if (listener.listener != null && listener.listener.asBinder().equals(token.asBinder())
                        && listenerAllowed(listener.component)
                        && java.util.Objects.equals(listener.identity,
                                codeIdentity(packageFor(listener.component.getPackageName())))) {
                    own(listener.component.getPackageName());
                    return listener;
                }
            }
        }
        throw new SecurityException("Unregistered or revoked notification listener");
    }

    /** Called outside PermissionPolicy's lock so revocation removes existing content. */
    void permissionChanged(String pkg) {
        if (!protectedMode() || mayPost(pkg)) return;
        synchronized (mPublicationLock) {
            if (mayPost(pkg)) return;
            synchronized (this) { mPosted.keySet().removeIf(k -> k.startsWith(pkg + "/")); }
            NotificationStore.deleteAll(pkg);
            if (mTestToast == null) WindowsHost.clearToasts(AppShell.appUserModelId(pkg));
        }
    }

    // --- posting ------------------------------------------------------------------------------

    /** Posts a notification (enqueueNotificationWithTag, or a foreground service's). */
    void post(String pkg, String tag, int id, Notification notification) {
        postFor(packageFor(pkg), pkg, tag, id, notification);
    }

    void postFor(ApkPackage captured, String pkg, String tag, int id, Notification notification) {
        if (notification == null) return;
        // Foreground-service state lives in ActiveServices; denied posting must not
        // expose its content through toasts, listeners or active-notification queries.
        if (!mayPost(pkg)) { permissionChanged(pkg); return; }
        final String channelKey = channelKey(pkg, notification.getChannelId());
        final NotificationChannel capturedChannel;
        final NotificationChannel channel;
        synchronized (this) {
            loadChannelsLocked(pkg);
            capturedChannel = mChannels.get(channelKey);
            if (capturedChannel == null || capturedChannel.isDeleted()) {
                SystemServer.log("notification: no channel " + notification.getChannelId() + " for " + pkg);
                return;
            }
            channel = capturedChannel;
        }
        StatusBarNotification sbn = new StatusBarNotification(pkg, pkg, id, tag,
                packageFor(pkg) == null ? android.os.Process.SYSTEM_UID : packageFor(pkg).uid(),
                0, notification, UserHandle.SYSTEM, null,
                System.currentTimeMillis());
        Record r = new Record(sbn, channel, notificationImportance(channel, notification,
                captured != null && captured.parsed.getTargetSdkVersion() < android.os.Build.VERSION_CODES.O));
        Runnable beforePublication = mBeforePostPublication;
        if (beforePublication != null) beforePublication.run();
        synchronized (mPublicationLock) {
            if (packageFor(pkg) != captured || captured == null && !"android".equals(pkg) || !mayPost(pkg)) return;
            boolean update;
            synchronized (this) {
                // Group/channel deletion and recreation must not authorize a post
                // that captured a previous channel generation before publication.
                if (mChannels.get(channelKey) != capturedChannel) return;
                NotificationChannelGroup group = channel.getGroup() == null ? null
                        : mChannelGroups.get(pkg + "/" + channel.getGroup());
                if ((r.importance == NotificationManager.IMPORTANCE_NONE || group != null && group.isBlocked())
                        && !notification.isMediaNotification()) return;
                update = mPosted.put(key(pkg, tag, id), r) != null;
            }
            int importance = r.importance;
            String shown;
            if (importance == NotificationManager.IMPORTANCE_NONE) {
                shown = "not shown: its channel is blocked";
            } else {
                // Its intents outlive this system server, as its toast does (ADR 0051).
                if (WindowsHost.inSystemServer()) {
                    NotificationStore.save(am(), sbn.getKey(), pkg, tag, id, notification);
                }
                int hr = mTestToast == null ? showToast(r, update) : mTestToast.applyAsInt(sbn, update);
                shown = hr == 0 ? "a toast of " + AppShell.appUserModelId(pkg)
                        : String.format("no toast (0x%08x)", hr);
            }
            SystemServer.log("notification " + id + (update ? " updated" : " posted") + " for "
                    + pkg + ": channel " + channel.getId() + " (importance " + importance + ", " + shown + ")");
        }
        notifyListeners(r, true, 0);
    }

    void cancel(String pkg, String tag, int id) {
        cancel(pkg, tag, id, NotificationListenerService.REASON_APP_CANCEL);
    }

    /** The final UIJ association ended while its notification remains visible. */
    void removeUserInitiatedJobFlag(String pkg, int id) {
        Record record;
        synchronized (mPublicationLock) {
            synchronized (this) {
                record = mPosted.get(key(pkg, null, id));
                if (record == null || (record.n().flags & Notification.FLAG_USER_INITIATED_JOB) == 0) return;
                record.n().flags &= ~Notification.FLAG_USER_INITIATED_JOB;
            }
            if (WindowsHost.inSystemServer()) {
                NotificationStore.save(am(), record.sbn.getKey(), pkg, null, id, record.n());
            }
        }
        notifyListeners(record, true, 0);
    }

    private static int notificationImportance(NotificationChannel channel, Notification notification, boolean legacy) {
        int importance = channel.getImportance();
        if (!legacy || !DEFAULT_CHANNEL.equals(channel.getId())
                || importance != NotificationManager.IMPORTANCE_UNSPECIFIED && channel.hasUserSetImportance()) return importance;
        if ((notification.flags & Notification.FLAG_HIGH_PRIORITY) != 0) notification.priority = Notification.PRIORITY_MAX;
        notification.priority = Math.max(Notification.PRIORITY_MIN, Math.min(Notification.PRIORITY_MAX, notification.priority));
        importance = notification.priority == Notification.PRIORITY_MIN ? NotificationManager.IMPORTANCE_MIN
                : notification.priority == Notification.PRIORITY_LOW ? NotificationManager.IMPORTANCE_LOW
                : notification.priority >= Notification.PRIORITY_HIGH ? NotificationManager.IMPORTANCE_HIGH
                : NotificationManager.IMPORTANCE_DEFAULT;
        boolean sound = (channel.getUserLockedFields() & NotificationChannel.USER_LOCKED_SOUND) == 0
                ? (notification.defaults & Notification.DEFAULT_SOUND) != 0 || notification.sound != null
                : channel.getSound() != null;
        boolean vibration = channel.shouldVibrate();
        if ((channel.getUserLockedFields() & NotificationChannel.USER_LOCKED_VIBRATION) == 0) {
            vibration = (notification.defaults & Notification.DEFAULT_VIBRATE) != 0;
            if (!vibration && notification.vibrate != null) {
                try {
                    vibration = android.os.VibrationEffect.createWaveform(notification.vibrate,
                            (notification.flags & Notification.FLAG_INSISTENT) != 0 ? 0 : -1) != null;
                } catch (IllegalArgumentException invalidPattern) { vibration = false; }
            }
        }
        if (sound || vibration) importance = Math.max(importance, NotificationManager.IMPORTANCE_DEFAULT);
        else importance = Math.min(importance, NotificationManager.IMPORTANCE_LOW);
        if (notification.fullScreenIntent != null) importance = NotificationManager.IMPORTANCE_HIGH;
        return importance;
    }

    private void cancel(String pkg, String tag, int id, int reason) {
        Record r;
        synchronized (this) {
            r = mPosted.remove(key(pkg, tag, id));
        }
        String sbnKey = new StatusBarNotification(pkg, pkg, id, tag,
                packageFor(pkg) == null ? android.os.Process.SYSTEM_UID : packageFor(pkg).uid(),
                0, new Notification(), UserHandle.SYSTEM, null, 0).getKey();
        if (r == null) {
            // Posted by an earlier run of the device: its toast may still be in Windows'
            // notification center.
            if (NotificationStore.load(pkg, sbnKey) != null) {
                NotificationStore.delete(pkg, sbnKey);
                int hr = mTestToast == null ? WindowsHost.removeToast(AppShell.appUserModelId(pkg),
                        toastTag(tag, id), TOAST_GROUP) : 0;
                SystemServer.log("notification " + id + " of " + pkg + " (an earlier run's)"
                        + " cancelled" + (hr == 0 ? ", its toast removed"
                                : String.format(" (toast: 0x%08x)", hr)));
            }
            return;
        }
        NotificationStore.delete(pkg, r.sbn.getKey());
        int hr = mTestToast == null ? WindowsHost.cancelNotificationBridge(AppShell.appUserModelId(pkg), toastTag(tag, id),
                TOAST_GROUP, id) : 0;
        SystemServer.log("notification " + id + " of " + pkg + " cancelled"
                + (hr == 0 ? ", its toast removed" : String.format(" (toast: 0x%08x)", hr)));
        notifyListeners(r, false, reason);
    }

    private static String channelKey(String pkg, String id) {
        return pkg + "/" + (id != null ? id : DEFAULT_CHANNEL);
    }

    /** Trusted service/job association changes use the same order as channel deletion. */
    void withChannelLifecycle(Runnable action) {
        if (mDeferredListenerChanges.get() != null) {
            synchronized (mPublicationLock) { action.run(); }
            return;
        }
        List<Runnable> callbacks = new ArrayList<>();
        mDeferredListenerChanges.set(callbacks);
        try {
            synchronized (mPublicationLock) { action.run(); }
        } finally {
            mDeferredListenerChanges.remove();
            for (Runnable callback : callbacks) callback.run();
        }
    }

    void attachJobScheduler(JobSchedulerService jobs) {
        synchronized (mPublicationLock) { mJobScheduler = jobs; }
    }

    private int showToast(Record r, boolean update) {
        String pkg = r.sbn.getPackageName();
        String appId = AppShell.appUserModelId(pkg);
        startEvents();
        File icon = registerAppId(pkg, appId);
        Notification n = r.n();
        File large = NotificationIcons.largeIcon(pkg, r.sbn.getKey(), n.getLargeIcon());
        int importance = r.importance;
        boolean quiet = importance == NotificationManager.IMPORTANCE_MIN
                || importance == NotificationManager.IMPORTANCE_LOW;
        boolean suppressPopup = quiet
                || (update && (n.flags & Notification.FLAG_ONLY_ALERT_ONCE) != 0);
        boolean high = importance >= NotificationManager.IMPORTANCE_HIGH;
        String xml = toastXml(r, large != null ? large : icon, quiet || update);
        int hr = WindowsHost.showToast(appId, toastTag(r.sbn.getTag(), r.sbn.getId()),
                TOAST_GROUP, xml, suppressPopup, high);
        if (hr != 0) {
            // Fall back to Shell_NotifyIcon balloon notification bridge with app icon and title
            CharSequence title = n.extras != null ? n.extras.getCharSequence(Notification.EXTRA_TITLE) : null;
            CharSequence text = n.extras != null ? n.extras.getCharSequence(Notification.EXTRA_TEXT) : null;
            File iconFile = large != null ? large : icon;
            hr = WindowsHost.showNotificationBridge(
                    appId, toastTag(r.sbn.getTag(), r.sbn.getId()), TOAST_GROUP,
                    title != null ? title.toString() : "",
                    text != null ? text.toString() : "",
                    iconFile != null ? iconFile.getAbsolutePath() : "",
                    r.sbn.getId());
        }
        return hr;
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

    /**
     * The persistent device serves Gesher's toast activator (ADR 0051): clicks on its apps'
     * toasts come here, also for toasts an earlier run of the device posted.
     */
    void startActivator() {
        startEvents();
        int hr = WindowsHost.registerToastActivator(false);
        SystemServer.log(hr == 0
                ? "notification: serving the toast activator " + WindowsHost.TOAST_ACTIVATOR_CLSID
                : String.format("notification: the toast activator %s is not served (0x%08x)",
                        WindowsHost.TOAST_ACTIVATOR_CLSID, hr));
    }

    @Override
    public void onWindowsEvents(int events) {
        if ((events & WindowsHost.EVENT_TOAST_ACTIVATED) == 0) return;
        String[] a;
        while ((a = WindowsHost.nextActivation()) != null) {
            activated(a[0], a[3], a[1].isEmpty() ? "the toast activator" : "the toast");
        }
    }

    // The last click taken (a click may come both through the activator and the toast's
    // Activated event): guarded by this.
    private String mLastActivation;
    private long mLastActivationTime;

    /**
     * A click on a toast of {@code appId}, from {@code source} (the toast's Activated
     * event, the activator, or a launcher the activator started).
     */
    void activated(String appId, String arguments, String source) {
        long now = android.os.SystemClock.uptimeMillis();
        synchronized (this) {
            String what = appId + "\n" + arguments;
            if (what.equals(mLastActivation) && now - mLastActivationTime < 5000) return;
            mLastActivation = what;
            mLastActivationTime = now;
        }
        SystemServer.log("notification: a click on a toast of " + appId + " (through " + source
                + "): " + arguments);
        activated(arguments);
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
            activatedStored(key, action);
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

    /**
     * A click on a toast whose notification this system server did not post (an earlier
     * run of the device did; NotificationStore kept its intents).
     */
    private void activatedStored(String key, int action) {
        String[] parts = key.split("\\|", 5);  // user|package|id|tag|uid
        NotificationStore.Stored s = parts.length >= 3
                ? NotificationStore.load(parts[1], key) : null;
        if (s == null) {
            SystemServer.log("notification: a toast was clicked whose notification is gone");
            return;
        }
        NotificationStore.Intended intent = action < 0 ? s.content
                : action < s.actions.length ? s.actions[action] : null;
        SystemServer.log("notification " + s.id + " of " + s.packageName + " (an earlier run's):"
                + " its toast was clicked" + (action >= 0 ? " (action " + action + ")" : "")
                + (intent != null ? ", sending its " + PendingIntents.typeName(intent.type)
                        + " intent " + intent.intents[intent.intents.length - 1]
                        : ", nothing to send"));
        ActivityManagerService am = am();
        if (intent != null && am != null) {
            try {
                NotificationStore.send(am, intent);
            } catch (Throwable t) {
                SystemServer.log("notification: sending the stored intent failed: " + t);
            }
        }
        if (action < 0 && (s.flags & Notification.FLAG_AUTO_CANCEL) != 0) {
            NotificationStore.delete(s.packageName, key);
            WindowsHost.removeToast(AppShell.appUserModelId(s.packageName),
                    toastTag(s.tag, s.id), TOAST_GROUP);
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
            ComponentName component = new ComponentName(si.packageName, si.name);
            if (listenerAllowed(component)) bindListener(component);
        }
    }

    private void bindListener(ComponentName component) {
        ActivityManagerService am = am();
        if (am == null || !listenerAllowed(component)) return;
        synchronized (this) {
            for (Listener existing : mListeners) if (component.equals(existing.component)) return;
        }
        Listener l = new Listener(component);
        l.connection = new android.app.IServiceConnection.Stub() {
            @Override
            public void connected(ComponentName name, IBinder service,
                    android.app.IBinderSession session, boolean dead) {
                if (service == null) return;
                if (!listenerAllowed(component) || !java.util.Objects.equals(l.identity,
                        codeIdentity(packageFor(component.getPackageName())))) return;
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
        long identity = Binder.clearCallingIdentity();
        try {
            am.bindService(null, null, intent, null, l.connection, Context.BIND_AUTO_CREATE,
                    "android", 0);
            SystemServer.log("notification: binding listener " + component.flattenToShortString());
        } catch (Throwable t) {
            SystemServer.log("notification: binding " + component + " failed: " + t);
        } finally {
            Binder.restoreCallingIdentity(identity);
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
                if (l.connection != null) {
                    unbind.add(l);
                    mListeners.remove(i);
                }
            }
        }
        ActivityManagerService am = am();
        long identity = Binder.clearCallingIdentity();
        try {
        for (Listener l : unbind) {
            if (am != null) am.unbindService(l.connection);
        }
        } finally { Binder.restoreCallingIdentity(identity); }
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
                if (!mayPost(r.sbn.getPackageName())) continue;
                NotificationListenerService.Ranking ranking = new NotificationListenerService.Ranking();
                ranking.populate(r.sbn.getKey(), rank++, true /* matchesInterruptionFilter */,
                        NotificationListenerService.Ranking.VISIBILITY_NO_OVERRIDE,
                        0 /* suppressedVisualEffects */, r.importance,
                        null /* explanation */, null /* overrideGroupKey */, copyChannel(r.channel),
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
        List<Runnable> deferred = mDeferredListenerChanges.get();
        if (deferred != null) {
            deferred.add(() -> notifyListeners(r, posted, reason));
            return;
        }
        if (posted) synchronized (this) {
            if (mPosted.get(key(r.sbn.getPackageName(), r.sbn.getTag(), r.sbn.getId())) != r
                    || !mayPost(r.sbn.getPackageName())) return;
        }
        if (posted) bindListeners();
        List<INotificationListener> targets = new ArrayList<>();
        synchronized (this) {
            for (Listener l : mListeners) {
                if (l.listener != null && (!protectedMode() || (listenerAllowed(l.component)
                        && java.util.Objects.equals(l.identity,
                                codeIdentity(packageFor(l.component.getPackageName())))))) {
                    targets.add(l.listener);
                }
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
            if ((pkg == null || pkg.equals(r.sbn.getPackageName()))
                    && mayPost(r.sbn.getPackageName())) result.add(r.sbn);
        }
        return result;
    }

    // --- INotificationManager -------------------------------------------------------------------

    @Override
    public void enqueueNotificationWithTag(String pkg, String opPkg, String tag, int id,
            Notification notification, int userId) {
        ApkPackage captured = packageFor(pkg);
        user(userId); own(pkg); own(opPkg);
        if (!callers().isSystem() && !pkg.equals(opPkg)) throw new SecurityException("Invalid notification attribution");
        postFor(captured, pkg, tag, id, notification);
    }

    @Override
    public void cancelNotificationWithTag(String pkg, String opPkg, String tag, int id,
            int userId) {
        user(userId); own(pkg); own(opPkg);
        cancel(pkg, tag, id);
    }

    @Override
    public void cancelAllNotifications(String pkg, int userId) {
        user(userId); own(pkg);
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
            NotificationStore.delete(pkg, r.sbn.getKey());
            if (mTestToast == null) WindowsHost.removeToast(appId, toastTag(r.sbn.getTag(), r.sbn.getId()), TOAST_GROUP);
        }
        SystemServer.log("notification: all of " + pkg + " cancelled (" + removed.size() + ")");
        for (Record r : removed) {
            notifyListeners(r, false, NotificationListenerService.REASON_APP_CANCEL_ALL);
        }
    }

    /**
     * Removes every notification of the package, its toasts, the notifications kept for
     * clicks and its app id's registration (HKCU\Software\Classes\AppUserModelId): it is
     * uninstalled (Installer.uninstall).
     */
    void packageRemoved(String pkg) {
        synchronized (mPublicationLock) { packageRemovedLocked(pkg); }
    }
    private void packageRemovedLocked(String pkg) {
        synchronized (this) {
            AtomicFile file = channelFile(pkg);
            file.delete();
            for (String suffix : new String[]{"", ".new", ".bak"}) {
                if (new File(file.getBaseFile().getPath() + suffix).exists()) {
                    throw new IllegalStateException("Cannot remove notification channel state for " + pkg);
                }
            }
            mPosted.keySet().removeIf(k -> k.startsWith(pkg + "/"));
            mChannels.keySet().removeIf(k -> k.startsWith(pkg + "/"));
            mChannelGroups.keySet().removeIf(k -> k.startsWith(pkg + "/"));
            mLoadedChannelPackages.add(pkg);
        }
        revokeListenersForPackage(pkg);
        invalidateChannelCaches(true, true);
        String appId = AppShell.appUserModelId(pkg);
        int cleared = mTestToast == null ? WindowsHost.clearToasts(appId) : 0;
        int unregistered = mTestToast == null ? WindowsHost.unregisterAppId(appId) : 0;
        NotificationStore.deleteAll(pkg);
        synchronized (this) {
            mRegisteredAppIds.remove(appId);
        }
        SystemServer.log(String.format("notification: %s removed: its toasts (0x%08x) and %s's"
                + " registration (0x%08x)", pkg, cleared, appId, unregistered));
    }

    @Override
    public ParceledListSlice getAppActiveNotifications(String callingPkg, int userId) {
        user(userId); own(callingPkg);
        return new ParceledListSlice<>(active(callingPkg));
    }

    @Override
    public StatusBarNotification[] getActiveNotifications(String callingPkg) {
        own(callingPkg);
        return active(callers().isSystem() ? null : callingPkg).toArray(new StatusBarNotification[0]);
    }

    @Override
    public StatusBarNotification[] getActiveNotificationsWithAttribution(String callingPkg,
            String callingAttributionTag) {
        return getActiveNotifications(callingPkg);
    }

    @Override
    public ParceledListSlice getActiveNotificationsFromListener(INotificationListener token,
            String[] keys, int trim) {
        listenerAccess(token);
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
        user(userid);
        if (listener == null) throw new SecurityException("Missing listener Binder");
        if (component != null) own(component.getPackageName());
        if (protectedMode()) {
            if (component == null) throw new SecurityException("Missing listener component");
            if (!listenerAllowed(component)) throw new SecurityException("Notification listener access denied");
        }
        Listener l = new Listener(component);
        l.listener = listener;
        synchronized (this) {
            mListeners.removeIf(existing -> existing.listener != null
                    && existing.listener.asBinder().equals(listener.asBinder()));
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
        user(userid); listenerAccess(listener);
        synchronized (this) {
            mListeners.removeIf(l -> l.listener != null
                    && l.listener.asBinder().equals(listener.asBinder()));
        }
    }

    @Override
    public void requestBindListener(ComponentName component) {
        if (component == null) throw new IllegalArgumentException("Missing listener component");
        own(component.getPackageName());
        if (!listenerAllowed(component)) throw new SecurityException("Notification listener access denied");
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
        listenerAccess(token);
        cancel(pkg, tag, id, NotificationListenerService.REASON_CANCEL);
    }

    @Override
    public void cancelNotificationsFromListener(INotificationListener token, String[] keys) {
        listenerAccess(token);
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
        own(pkg);
        permissionChanged(pkg);
        return mayPost(pkg);
    }

    @Override
    public boolean areNotificationsEnabledForPackage(String pkg, int uid) {
        own(pkg);
        ApkPackage installed = packageFor(pkg);
        if (installed == null || installed.uid() != uid) return false;
        return areNotificationsEnabled(pkg);
    }

    private static AtomicFile channelFile(String pkg) {
        return new AtomicFile(new File(Device.dataDir(), "system/notification-channels/" + pkg + ".xml"));
    }

    /** Framework XML preserves channel policy, including deleted-channel tombstones. */
    private void loadChannelsLocked(String pkg) {
        if (mLoadedChannelPackages.contains(pkg)) {
            updateDefaultChannelLocked(pkg);
            return;
        }
        Map<String, NotificationChannel> channels = new LinkedHashMap<>();
        Map<String, NotificationChannelGroup> groups = new LinkedHashMap<>();
        try (FileInputStream input = channelFile(pkg).openRead()) {
            TypedXmlPullParser parser = Xml.resolvePullParser(input);
            int event;
            do { event = parser.next(); } while (event != XmlPullParser.START_TAG
                    && event != XmlPullParser.END_DOCUMENT);
            ApkPackage installed = packageFor(pkg);
            int uid = installed == null ? android.os.Process.SYSTEM_UID : installed.uid();
            if (event != XmlPullParser.START_TAG || !"notification-channels".equals(parser.getName())
                    || parser.getAttributeInt(null, "version", -1) != 1
                    || !pkg.equals(parser.getAttributeValue(null, "package"))
                    || parser.getAttributeInt(null, "uid", -1) != uid) {
                throw new IOException("Wrong notification channel state owner or version");
            }
            while ((event = parser.next()) != XmlPullParser.END_DOCUMENT) {
                if (event != XmlPullParser.START_TAG || parser.getDepth() != 2) continue;
                String id = parser.getAttributeValue(null, "id");
                String name = parser.getAttributeValue(null, "name");
                if (id == null || name == null || name.isEmpty()) throw new IOException("Invalid channel header");
                if ("channelGroup".equals(parser.getName())) {
                    if (groups.size() >= GROUP_LIMIT) throw new IOException("Too many channel groups");
                    NotificationChannelGroup group = new NotificationChannelGroup(id, name);
                    group.populateFromXml(parser);
                    group.lockFields(parser.getAttributeInt(null, "locked", 0));
                    groups.put(id, group);
                } else if ("channel".equals(parser.getName())) {
                    if (channels.size() >= CHANNEL_LIMIT) throw new IOException("Too many channels");
                    int importance = parser.getAttributeInt(null, "importance", NotificationManager.IMPORTANCE_UNSPECIFIED);
                    if (importance < NotificationManager.IMPORTANCE_NONE
                            && !(DEFAULT_CHANNEL.equals(id) && importance == NotificationManager.IMPORTANCE_UNSPECIFIED)
                            || importance > NotificationManager.IMPORTANCE_MAX) throw new IOException("Invalid channel importance");
                    NotificationChannel channel = new NotificationChannel(id, name, importance);
                    channel.populateFromXml(parser);
                    // PreferencesHelper expires deleted channel policy after 30 days.
                    if (!channel.isDeleted() || channel.getDeletedTimeMs()
                            > System.currentTimeMillis() - java.util.concurrent.TimeUnit.DAYS.toMillis(30)) {
                        channels.put(id, channel);
                    }
                }
            }
            putChannelStateLocked(pkg, channels, groups);
        } catch (java.io.FileNotFoundException absent) {
            // A package that has never created channels has no policy file.
        } catch (IOException | org.xmlpull.v1.XmlPullParserException | RuntimeException invalid) {
            SystemServer.log("notification: cannot load channels for " + pkg + ": " + invalid);
        }
        mLoadedChannelPackages.add(pkg);
        updateDefaultChannelLocked(pkg);
    }

    private void updateDefaultChannelLocked(String pkg) {
        ApkPackage installed = packageFor(pkg);
        boolean legacy = installed != null && !pm().isRemovalPending(pkg)
                && installed.parsed.getTargetSdkVersion() < android.os.Build.VERSION_CODES.O;
        String key = channelKey(pkg, DEFAULT_CHANNEL);
        NotificationChannel existing = mChannels.get(key);
        if (legacy == (existing != null)) return;
        Map<String, NotificationChannel> channels = channelsLocked(pkg);
        if (legacy) {
            NotificationChannel channel = new NotificationChannel(DEFAULT_CHANNEL,
                    android.content.res.Resources.getSystem().getString(com.android.internal.R.string.default_notification_channel_label),
                    NotificationManager.IMPORTANCE_UNSPECIFIED);
            channels.put(DEFAULT_CHANNEL, channel);
        } else {
            channels.remove(DEFAULT_CHANNEL);
        }
        commitChannelStateLocked(pkg, channels, groupsLocked(pkg));
        invalidateChannelCaches(true, false);
    }

    private Map<String, NotificationChannel> channelsLocked(String pkg) {
        Map<String, NotificationChannel> result = new LinkedHashMap<>();
        for (Map.Entry<String, NotificationChannel> entry : mChannels.entrySet()) {
            if (entry.getKey().startsWith(pkg + "/")) result.put(entry.getValue().getId(), entry.getValue());
        }
        return result;
    }

    private Map<String, NotificationChannelGroup> groupsLocked(String pkg) {
        Map<String, NotificationChannelGroup> result = new LinkedHashMap<>();
        for (Map.Entry<String, NotificationChannelGroup> entry : mChannelGroups.entrySet()) {
            if (entry.getKey().startsWith(pkg + "/")) result.put(entry.getValue().getId(), entry.getValue());
        }
        return result;
    }

    private void putChannelStateLocked(String pkg, Map<String, NotificationChannel> channels,
            Map<String, NotificationChannelGroup> groups) {
        mChannels.keySet().removeIf(k -> k.startsWith(pkg + "/"));
        mChannelGroups.keySet().removeIf(k -> k.startsWith(pkg + "/"));
        for (NotificationChannel channel : channels.values()) mChannels.put(channelKey(pkg, channel.getId()), channel);
        for (NotificationChannelGroup group : groups.values()) mChannelGroups.put(pkg + "/" + group.getId(), group);
    }

    /** Save a complete candidate before exposing it; failed writes leave the old maps intact. */
    private void commitChannelStateLocked(String pkg, Map<String, NotificationChannel> channels,
            Map<String, NotificationChannelGroup> groups) {
        if (channels.equals(channelsLocked(pkg)) && groups.equals(groupsLocked(pkg))) return;
        AtomicFile file = channelFile(pkg);
        FileOutputStream output = null;
        try {
            output = file.startWrite();
            TypedXmlSerializer xml = Xml.resolveSerializer(output);
            xml.startDocument(null, true);
            xml.startTag(null, "notification-channels");
            xml.attributeInt(null, "version", 1);
            xml.attribute(null, "package", pkg);
            ApkPackage installed = packageFor(pkg);
            xml.attributeInt(null, "uid", installed == null ? android.os.Process.SYSTEM_UID : installed.uid());
            for (NotificationChannelGroup group : groups.values()) group.writeXml(xml);
            for (NotificationChannel channel : channels.values()) channel.writeXml(xml);
            xml.endTag(null, "notification-channels");
            xml.endDocument();
            xml.flush();
            output.getFD().sync();
            file.finishWrite(output);
            if (new File(file.getBaseFile().getPath() + ".new").exists()) {
                // AtomicFile logs a failed rename instead of throwing. Do not
                // publish an in-memory policy that was not committed to disk.
                throw new IOException("Atomic notification channel commit failed");
            }
            output = null;
        } catch (IOException | RuntimeException error) {
            if (output != null) file.failWrite(output);
            throw new IllegalStateException("Cannot save notification channels for " + pkg, error);
        }
        putChannelStateLocked(pkg, channels, groups);
    }

    private static void invalidateChannelCaches(boolean channels, boolean groups) {
        if (android.app.Flags.nmBinderPerfCacheChannels()) {
            if (channels) NotificationManager.invalidateNotificationChannelCache();
            if (groups) NotificationManager.invalidateNotificationChannelGroupCache();
        }
    }

    private static NotificationChannel copyChannel(NotificationChannel channel) {
        NotificationChannel copy = channel.copy();
        // copy() normalizes a null effect by clearing its pattern. Android's
        // pattern setter can retain an invalid pattern with a null effect.
        long[] pattern = channel.getVibrationPattern();
        android.os.VibrationEffect effect = channel.getVibrationEffect();
        if (pattern != null) copy.setVibrationPattern(pattern.clone());
        if (android.app.Flags.notificationChannelVibrationEffectApi() && effect != null) {
            if (effect instanceof android.os.VibrationEffect.Composed composed) {
                effect = new android.os.VibrationEffect.Composed(composed.getSegments(), composed.getRepeatIndex());
            } else if (effect instanceof android.os.VibrationEffect.VendorEffect vendor) {
                effect = new android.os.VibrationEffect.VendorEffect(vendor.getVendorData().deepCopy(),
                        vendor.getEffectStrength(), vendor.getScale(), vendor.getAdaptiveScale());
            }
            copy.setVibrationEffect(effect);
        }
        copy.enableVibration(channel.shouldVibrate());
        return copy;
    }

    private static NotificationChannelGroup copyGroup(NotificationChannelGroup group) {
        NotificationChannelGroup copy = new NotificationChannelGroup(group.getId(), group.getName());
        copy.setDescription(group.getDescription());
        copy.setBlocked(group.isBlocked());
        copy.lockFields(group.getUserLockedFields());
        for (NotificationChannel channel : group.getChannels()) copy.addChannel(copyChannel(channel));
        return copy;
    }

    private void channelOwnerCurrent(String pkg, ApkPackage captured) {
        if (packageFor(pkg) != captured || pm() != null && pm().isRemovalPending(pkg)) {
            throw new SecurityException("Notification owner is being removed");
        }
    }

    @Override
    public void createNotificationChannels(String pkg, ParceledListSlice channelsList) {
        own(pkg);
        ApkPackage captured = packageFor(pkg);
        Objects.requireNonNull(channelsList);
        synchronized (mPublicationLock) {
            channelOwnerCurrent(pkg, captured);
            synchronized (this) {
                loadChannelsLocked(pkg);
                Map<String, NotificationChannel> channels = channelsLocked(pkg);
                Map<String, NotificationChannelGroup> groups = groupsLocked(pkg);
                for (Object value : channelsList.getList()) {
                    NotificationChannel requested = Objects.requireNonNull((NotificationChannel) value);
                    Objects.requireNonNull(requested.getId());
                    if (requested.getName() == null || requested.getName().length() == 0
                            || requested.getImportance() < NotificationManager.IMPORTANCE_NONE
                            || requested.getImportance() > NotificationManager.IMPORTANCE_MAX) {
                        throw new IllegalArgumentException("Invalid notification channel name or importance");
                    }
                    if (DEFAULT_CHANNEL.equals(requested.getId())) throw new IllegalArgumentException("Reserved channel id");
                    if (requested.getGroup() != null && !groups.containsKey(requested.getGroup())) {
                        throw new IllegalArgumentException("NotificationChannelGroup doesn't exist");
                    }
                    NotificationChannel old = channels.get(requested.getId());
                    NotificationChannel channel;
                    if (old == null) {
                        if (channels.size() >= CHANNEL_LIMIT) throw new IllegalStateException("Too many notification channels");
                        if (requested.getParentChannelId() != null && !channels.containsKey(requested.getParentChannelId())) {
                            throw new IllegalArgumentException("Conversation channel requires a preexisting parent");
                        }
                        channel = copyChannel(requested);
                        channel.unlockFields(channel.getUserLockedFields());
                        channel.setBypassDnd(false);
                        channel.setLockscreenVisibility(NotificationListenerService.Ranking.VISIBILITY_NO_OVERRIDE);
                        channel.setAllowBubbles(NotificationChannel.DEFAULT_ALLOW_BUBBLE);
                        channel.setImportantConversation(false);
                        channel.setOriginalImportance(channel.getImportance());
                    } else {
                        // Android allows metadata updates and an importance downgrade;
                        // recreating a channel cannot reset its user policy or move it.
                        channel = copyChannel(old);
                        channel.setName(requested.getName().toString());
                        channel.setDescription(requested.getDescription());
                        channel.setBlockable(requested.isBlockable());
                        if (old.getGroup() == null && requested.getGroup() != null) channel.setGroup(requested.getGroup());
                        if (old.getUserLockedFields() == 0 && requested.getImportance() < old.getImportance()) {
                            channel.setImportance(requested.getImportance());
                        }
                    }
                    channel.setDeleted(false);
                    channel.setDeletedTimeMs(-1);
                    channels.put(channel.getId(), channel.equals(old) ? old : channel);
                }
                commitChannelStateLocked(pkg, channels, groups);
            }
            invalidateChannelCaches(true, false);
        }
    }

    @Override
    public NotificationChannel getNotificationChannel(String callingPkg, int userId,
            String pkg, String channelId) {
        user(userId); own(callingPkg); own(pkg);
        synchronized (this) {
            loadChannelsLocked(pkg);
            NotificationChannel channel = mChannels.get(channelKey(pkg, channelId));
            return channel == null || channel.isDeleted() ? null : copyChannel(channel);
        }
    }

    @Override
    public ParceledListSlice getNotificationChannels(String callingPkg, String targetPkg,
            int userId) {
        user(userId); own(callingPkg); own(targetPkg);
        List<NotificationChannel> result = new ArrayList<>();
        synchronized (this) {
            loadChannelsLocked(targetPkg);
            for (Map.Entry<String, NotificationChannel> e : mChannels.entrySet()) {
                if (e.getKey().startsWith(targetPkg + "/") && !e.getValue().isDeleted()) result.add(copyChannel(e.getValue()));
            }
        }
        return new ParceledListSlice<>(result);
    }

    @Override
    public void deleteNotificationChannel(String pkg, String channelId) {
        own(pkg);
        if (DEFAULT_CHANNEL.equals(channelId)) throw new IllegalArgumentException("Cannot delete default channel");
        ApkPackage captured = packageFor(pkg);
        List<Record> removed;
        synchronized (mPublicationLock) {
            channelOwnerCurrent(pkg, captured);
            enforceChannelNotInUse(pkg, channelId);
            synchronized (this) {
                loadChannelsLocked(pkg);
                Map<String, NotificationChannel> channels = channelsLocked(pkg);
                NotificationChannel channel = channels.get(channelId);
                if (channel != null && !channel.isDeleted()) {
                    NotificationChannel deleted = copyChannel(channel);
                    deleted.setDeleted(true);
                    deleted.setDeletedTimeMs(System.currentTimeMillis());
                    channels.put(channelId, deleted);
                    commitChannelStateLocked(pkg, channels, groupsLocked(pkg));
                }
                removed = removeChannelNotificationsLocked(pkg, java.util.Collections.singleton(channelId));
            }
            invalidateChannelCaches(true, false);
            removeChannelToasts(removed);
            removeRetainedChannelToasts(pkg, java.util.Collections.singleton(channelId));
        }
        notifyChannelRemoved(removed);
    }

    @Override
    public void createNotificationChannelGroups(String pkg, ParceledListSlice groupsList) {
        own(pkg);
        ApkPackage captured = packageFor(pkg);
        Objects.requireNonNull(groupsList);
        synchronized (mPublicationLock) {
            channelOwnerCurrent(pkg, captured);
            synchronized (this) {
                loadChannelsLocked(pkg);
                Map<String, NotificationChannelGroup> groups = groupsLocked(pkg);
                for (Object value : groupsList.getList()) {
                    NotificationChannelGroup requested = Objects.requireNonNull((NotificationChannelGroup) value);
                    Objects.requireNonNull(requested.getId());
                    if (requested.getName() == null || requested.getName().length() == 0) {
                        throw new IllegalArgumentException("Notification channel group name is empty");
                    }
                    NotificationChannelGroup old = groups.get(requested.getId());
                    if (old == null && groups.size() >= GROUP_LIMIT) throw new IllegalStateException("Too many notification channel groups");
                    // Only metadata is app-writable. Never accept an app-supplied
                    // blocked policy, user-lock fields or embedded member channels.
                    NotificationChannelGroup group = new NotificationChannelGroup(requested.getId(), requested.getName().toString());
                    group.setDescription(requested.getDescription());
                    if (old != null) {
                        group.setBlocked(old.isBlocked());
                        group.lockFields(old.getUserLockedFields());
                    }
                    groups.put(group.getId(), group);
                }
                commitChannelStateLocked(pkg, channelsLocked(pkg), groups);
            }
            invalidateChannelCaches(false, true);
        }
    }

    @Override
    public NotificationChannelGroup getNotificationChannelGroup(String pkg, String groupId) {
        own(pkg);
        if (groupId == null) return null;
        synchronized (this) {
            loadChannelsLocked(pkg);
            NotificationChannelGroup group = NotificationChannelGroupsHelper.getGroupWithChannels(
                    groupId, channelsLocked(pkg).values(), groupsLocked(pkg), false);
            return group == null ? null : copyGroup(group);
        }
    }

    @Override
    public ParceledListSlice<NotificationChannelGroup> getNotificationChannelGroups(String pkg) {
        own(pkg);
        synchronized (this) {
            loadChannelsLocked(pkg);
            List<NotificationChannelGroup> result = new ArrayList<>();
            for (NotificationChannelGroup group : NotificationChannelGroupsHelper.getGroupsWithChannels(
                    channelsLocked(pkg).values(), groupsLocked(pkg), NotificationChannelGroupsHelper.Params.forAllGroups())) {
                result.add(copyGroup(group));
            }
            return new ParceledListSlice<>(result);
        }
    }

    @Override
    public ParceledListSlice<NotificationChannelGroup> getNotificationChannelGroupsWithoutChannels(String pkg) {
        own(pkg);
        synchronized (this) {
            loadChannelsLocked(pkg);
            List<NotificationChannelGroup> result = new ArrayList<>();
            for (NotificationChannelGroup group : groupsLocked(pkg).values()) result.add(copyGroup(group));
            return new ParceledListSlice<>(result);
        }
    }

    @Override
    public void deleteNotificationChannelGroup(String pkg, String groupId) {
        own(pkg);
        if (groupId == null || groupId.isEmpty()) return;
        ApkPackage captured = packageFor(pkg);
        List<Record> removed;
        synchronized (mPublicationLock) {
            channelOwnerCurrent(pkg, captured);
            Map<String, NotificationChannel> channels;
            Map<String, NotificationChannelGroup> groups;
            Set<String> memberIds = new HashSet<>();
            synchronized (this) {
                loadChannelsLocked(pkg);
                groups = groupsLocked(pkg);
                if (groups.remove(groupId) == null) return;
                channels = channelsLocked(pkg);
                for (NotificationChannel channel : channels.values()) {
                    if (groupId.equals(channel.getGroup()) && !channel.isDeleted()) memberIds.add(channel.getId());
                }
            }
            // Every member is checked before any channel/group or notification changes.
            for (String id : memberIds) enforceChannelNotInUse(pkg, id);
            synchronized (this) {
                for (String id : memberIds) {
                    NotificationChannel deleted = copyChannel(channels.get(id));
                    deleted.setDeleted(true);
                    deleted.setDeletedTimeMs(System.currentTimeMillis());
                    channels.put(id, deleted);
                }
                commitChannelStateLocked(pkg, channels, groups);
                removed = removeChannelNotificationsLocked(pkg, memberIds);
            }
            invalidateChannelCaches(!memberIds.isEmpty(), true);
            removeChannelToasts(removed);
            removeRetainedChannelToasts(pkg, memberIds);
        }
        notifyChannelRemoved(removed);
    }

    private void enforceChannelNotInUse(String pkg, String id) {
        ActivityManagerService activity = am();
        if (activity != null && activity.hasForegroundServiceNotification(pkg, id)
                || mJobScheduler != null && mJobScheduler.hasUserInitiatedJobNotification(pkg, id)) {
            throw new SecurityException("Cannot delete notification channel " + id + " while it is used by an active service or job");
        }
    }

    private List<Record> removeChannelNotificationsLocked(String pkg, Set<String> channels) {
        List<Record> removed = new ArrayList<>();
        mPosted.entrySet().removeIf(entry -> {
            Record record = entry.getValue();
            if (!pkg.equals(record.sbn.getPackageName()) || !channels.contains(record.channel.getId())) return false;
            removed.add(record);
            return true;
        });
        return removed;
    }

    private void removeChannelToasts(List<Record> removed) {
        for (Record record : removed) {
            String pkg = record.sbn.getPackageName();
            NotificationStore.delete(pkg, record.sbn.getKey());
            if (mTestToast == null) WindowsHost.cancelNotificationBridge(AppShell.appUserModelId(pkg),
                    toastTag(record.sbn.getTag(), record.sbn.getId()), TOAST_GROUP, record.sbn.getId());
        }
    }

    private void removeRetainedChannelToasts(String pkg, Set<String> channelIds) {
        for (NotificationStore.Stored stored : NotificationStore.deleteChannels(pkg, channelIds)) {
            if (mTestToast == null) WindowsHost.cancelNotificationBridge(AppShell.appUserModelId(pkg),
                    toastTag(stored.tag, stored.id), TOAST_GROUP, stored.id);
        }
    }

    private void notifyChannelRemoved(List<Record> removed) {
        for (Record record : removed) notifyListeners(record, false, NotificationListenerService.REASON_CHANNEL_REMOVED);
    }

    @Override
    public boolean isNotificationListenerAccessGranted(ComponentName component) {
        if (component == null) return false;
        own(component.getPackageName());
        return listenerAllowed(component);
    }

    @Override
    public boolean isNotificationListenerAccessGrantedForUser(ComponentName component, int userId) {
        user(userId);
        return isNotificationListenerAccessGranted(component);
    }

    @Override
    public void setNotificationListenerAccessGranted(ComponentName component, boolean granted,
            boolean userSet) {
        setNotificationListenerAccessGrantedForUser(component, 0, granted, userSet);
    }

    @Override
    public void setNotificationListenerAccessGrantedForUser(ComponentName component, int userId,
            boolean granted, boolean userSet) {
        ApkPackage captured = component == null ? null : packageFor(component.getPackageName());
        system(); user(userId);
        if (component == null || (granted && listenerService(component) == null)) {
            throw new IllegalArgumentException("Not a notification listener service");
        }
        String identity = granted ? codeIdentity(packageFor(component.getPackageName())) : null;
        if (granted && identity == null) throw new SecurityException("Unknown listener code identity");
        synchronized (this) {
            if (granted && (packageFor(component.getPackageName()) != captured
                    || pm() != null && pm().isRemovalPending(component.getPackageName()))) {
                throw new SecurityException("Notification listener changed or is retiring");
            }
            Object previous = granted ? mListenerGrants.setProperty(component.flattenToString(), identity)
                    : mListenerGrants.remove(component.flattenToString());
            try { Device.write(mListenerFile, mListenerGrants); }
            catch (java.io.IOException failure) {
                if (previous == null) mListenerGrants.remove(component.flattenToString());
                else mListenerGrants.put(component.flattenToString(), previous);
                throw new IllegalStateException("Could not persist notification listener decision", failure);
            }
        }
        if (granted) {
            // An explicit host decision can enable a new listener after the initial scan.
            synchronized (this) { if (mPosted.isEmpty()) return; }
            bindListener(component);
        } else {
            removeListeners(component);
        }
    }

    private void removeListeners(ComponentName component) {
        List<Listener> removed = new ArrayList<>();
        synchronized (this) {
            mListeners.removeIf(listener -> {
                if (!component.equals(listener.component)) return false;
                removed.add(listener); return true;
            });
        }
        ActivityManagerService am = am();
        long identity = Binder.clearCallingIdentity();
        try {
            for (Listener listener : removed) {
                if (listener.connection != null && am != null) am.unbindService(listener.connection);
            }
        } finally { Binder.restoreCallingIdentity(identity); }
    }

    private void revokeListenersForPackage(String pkg) {
        List<ComponentName> removed = new ArrayList<>();
        synchronized (this) {
            Properties before = new Properties(); before.putAll(mListenerGrants);
            for (Object key : new ArrayList<>(mListenerGrants.keySet())) {
                ComponentName component = ComponentName.unflattenFromString(key.toString());
                if (component != null && pkg.equals(component.getPackageName())) {
                    mListenerGrants.remove(key); removed.add(component);
                }
            }
            try { Device.write(mListenerFile, mListenerGrants); }
            catch (java.io.IOException failure) {
                mListenerGrants.clear(); mListenerGrants.putAll(before);
                throw new IllegalStateException("Could not revoke notification listener access", failure);
            }
            for (Listener listener : mListeners) {
                if (listener.component != null && pkg.equals(listener.component.getPackageName())
                        && !removed.contains(listener.component)) removed.add(listener.component);
            }
        }
        for (ComponentName component : removed) removeListeners(component);
    }

    @Override
    public List<ComponentName> getEnabledNotificationListeners(int userId) {
        system(); user(userId);
        List<ComponentName> result = new ArrayList<>();
        synchronized (this) {
            for (Object key : mListenerGrants.keySet()) {
                ComponentName component = ComponentName.unflattenFromString(key.toString());
                if (listenerAllowed(component)) result.add(component);
            }
        }
        return result;
    }

    @Override
    public List<String> getEnabledNotificationListenerPackages() {
        List<String> result = new ArrayList<>();
        for (ComponentName component : getEnabledNotificationListeners(0)) {
            if (!result.contains(component.getPackageName())) result.add(component.getPackageName());
        }
        return result;
    }

    @Override
    public boolean hasEnabledNotificationListener(String pkg, int userId) {
        user(userId); own(pkg);
        if (!protectedMode()) return true;
        synchronized (this) {
            for (Object key : mListenerGrants.keySet()) {
                ComponentName component = ComponentName.unflattenFromString(key.toString());
                if (component != null && pkg.equals(component.getPackageName()) && listenerAllowed(component)) return true;
            }
        }
        return false;
    }

    @Override
    public void requestUnbindListener(INotificationListener listener) {
        Listener registered = listenerAccess(listener);
        if (registered != null) removeListeners(registered.component);
        else unregisterListener(listener, 0);
    }

    @Override
    public void requestUnbindListenerComponent(ComponentName component) {
        if (component == null) throw new IllegalArgumentException("Missing listener component");
        own(component.getPackageName());
        removeListeners(component);
    }

    @Override
    public boolean canNotifyAsPackage(String callingPkg, String targetPkg, int userId) {
        user(userId); own(callingPkg);
        return callers().isSystem() || callingPkg.equals(targetPkg);
    }

    @Override
    public void createNotificationChannelsForPackage(String pkg, int uid, ParceledListSlice channels) {
        system(); createNotificationChannels(pkg, channels);
    }

    @Override
    public void updateNotificationChannelForPackage(String pkg, int uid, NotificationChannel channel) {
        system(); own(pkg);
        Objects.requireNonNull(channel); Objects.requireNonNull(channel.getId());
        ApkPackage captured = packageFor(pkg);
        if (uid != (captured == null ? android.os.Process.SYSTEM_UID : captured.uid())) {
            throw new SecurityException("Wrong notification channel owner UID");
        }
        List<Record> removed = new ArrayList<>();
        synchronized (mPublicationLock) {
            channelOwnerCurrent(pkg, captured);
            synchronized (this) {
                loadChannelsLocked(pkg);
                Map<String, NotificationChannel> channels = channelsLocked(pkg);
                NotificationChannel old = channels.get(channel.getId());
                if (old == null || old.isDeleted()) throw new IllegalArgumentException("Channel does not exist");
                NotificationChannel updated = copyChannel(channel);
                if (updated.getLockscreenVisibility() == Notification.VISIBILITY_PUBLIC) {
                    updated.setLockscreenVisibility(NotificationListenerService.Ranking.VISIBILITY_NO_OVERRIDE);
                }
                updated.lockFields(old.getUserLockedFields());
                lockChannelChanges(old, updated);
                if (old.isImportanceLockedByCriticalDeviceFunction()
                        && !(old.isBlockable() || old.getImportance() == NotificationManager.IMPORTANCE_NONE)) {
                    updated.setImportance(old.getImportance());
                }
                channels.put(updated.getId(), updated.equals(old) ? old : updated);
                commitChannelStateLocked(pkg, channels, groupsLocked(pkg));
                if (channel.getImportance() == NotificationManager.IMPORTANCE_NONE) {
                    removed = removeChannelNotificationsLocked(pkg, java.util.Collections.singleton(updated.getId()));
                }
            }
            invalidateChannelCaches(true, false);
            removeChannelToasts(removed);
            if (channel.getImportance() == NotificationManager.IMPORTANCE_NONE) {
                removeRetainedChannelToasts(pkg, java.util.Collections.singleton(channel.getId()));
            }
        }
        for (Record record : removed) notifyListeners(record, false, NotificationListenerService.REASON_CHANNEL_BANNED);
    }

    private static void lockChannelChanges(NotificationChannel old, NotificationChannel updated) {
        if (old.canBypassDnd() != updated.canBypassDnd()) updated.lockFields(NotificationChannel.USER_LOCKED_PRIORITY);
        if (old.getLockscreenVisibility() != updated.getLockscreenVisibility()) updated.lockFields(NotificationChannel.USER_LOCKED_VISIBILITY);
        if (old.getImportance() != updated.getImportance()) updated.lockFields(NotificationChannel.USER_LOCKED_IMPORTANCE);
        if (old.shouldShowLights() != updated.shouldShowLights() || old.getLightColor() != updated.getLightColor()) {
            updated.lockFields(NotificationChannel.USER_LOCKED_LIGHTS);
        }
        if (!Objects.equals(old.getSound(), updated.getSound())) updated.lockFields(NotificationChannel.USER_LOCKED_SOUND);
        if (!java.util.Arrays.equals(old.getVibrationPattern(), updated.getVibrationPattern())
                || !Objects.equals(old.getVibrationEffect(), updated.getVibrationEffect())
                || old.shouldVibrate() != updated.shouldVibrate()) updated.lockFields(NotificationChannel.USER_LOCKED_VIBRATION);
        if (old.canShowBadge() != updated.canShowBadge()) updated.lockFields(NotificationChannel.USER_LOCKED_SHOW_BADGE);
        if (old.getAllowBubbles() != updated.getAllowBubbles()) updated.lockFields(NotificationChannel.USER_LOCKED_ALLOW_BUBBLE);
    }

    @Override
    public NotificationChannel getNotificationChannelForPackage(String pkg, int uid, String channelId,
            String conversationId, boolean includeDeleted) {
        own(pkg); return getNotificationChannel(pkg, 0, pkg, channelId);
    }

    @Override
    public ParceledListSlice getNotificationChannelsForPackage(String pkg, int uid, boolean includeDeleted) {
        own(pkg); return getNotificationChannels(pkg, pkg, 0);
    }

    @Override
    public void setNotificationsEnabledForPackage(String pkg, int uid, boolean enabled) {
        system(); own(pkg);
        ApkPackage installed = packageFor(pkg);
        if (installed == null || installed.uid() != uid) throw new SecurityException("Wrong package UID");
        if (enabled) pm().permissions().grantRuntimePermission(pkg,
                android.Manifest.permission.POST_NOTIFICATIONS, "default:0", 0);
        else pm().permissions().revokeRuntimePermission(pkg,
                android.Manifest.permission.POST_NOTIFICATIONS, "default:0", 0, "host notification settings");
    }

    @Override
    public void setNotificationsEnabledWithImportanceLockForPackage(String pkg, int uid, boolean enabled) {
        setNotificationsEnabledForPackage(pkg, uid, enabled);
    }

    @Override
    public int getPackageImportance(String pkg) {
        return areNotificationsEnabled(pkg) ? NotificationManager.IMPORTANCE_DEFAULT
                : NotificationManager.IMPORTANCE_NONE;
    }
}
