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

import com.gesher.app.shell.AppShell;

import java.io.File;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
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
        final ComponentName component;
        final String identity;
        INotificationListener listener;
        android.app.IServiceConnection connection;

        Listener(ComponentName component) {
            this.component = component;
            this.identity = component == null ? null : codeIdentity(packageFor(component.getPackageName()));
        }
    }

    // Guarded by this. Keys: package/channel id; package/tag/id.
    private final Map<String, NotificationChannel> mChannels = new LinkedHashMap<>();
    private final Map<String, Record> mPosted = new LinkedHashMap<>();
    private final Object mPublicationLock = new Object();
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
        NotificationChannel channel = channelFor(pkg, notification.getChannelId());
        StatusBarNotification sbn = new StatusBarNotification(pkg, pkg, id, tag,
                packageFor(pkg) == null ? android.os.Process.SYSTEM_UID : packageFor(pkg).uid(),
                0, notification, UserHandle.SYSTEM, null,
                System.currentTimeMillis());
        Record r = new Record(sbn, channel);
        Runnable beforePublication = mBeforePostPublication;
        if (beforePublication != null) beforePublication.run();
        synchronized (mPublicationLock) {
            if (packageFor(pkg) != captured || captured == null && !"android".equals(pkg) || !mayPost(pkg)) return;
            boolean update;
            synchronized (this) {
                update = mPosted.put(key(pkg, tag, id), r) != null;
            }
            int importance = channel.getImportance();
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
        revokeListenersForPackage(pkg);
        synchronized (this) {
            mPosted.keySet().removeIf(k -> k.startsWith(pkg + "/"));
            mChannels.keySet().removeIf(k -> k.startsWith(pkg + "/"));
        }
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

    @Override
    public void createNotificationChannels(String pkg, ParceledListSlice channelsList) {
        own(pkg);
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
        user(userId); own(callingPkg); own(pkg);
        synchronized (this) {
            return mChannels.get(pkg + "/" + channelId);
        }
    }

    @Override
    public ParceledListSlice getNotificationChannels(String callingPkg, String targetPkg,
            int userId) {
        user(userId); own(callingPkg); own(targetPkg);
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
        own(pkg);
        synchronized (this) {
            mChannels.remove(pkg + "/" + channelId);
        }
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
        system(); createNotificationChannels(pkg, new ParceledListSlice<>(List.of(channel)));
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
