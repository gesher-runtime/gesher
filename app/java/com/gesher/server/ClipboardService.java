/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.Manifest;
import android.app.AppOpsManager;
import android.content.ClipData;
import android.content.ClipDescription;
import android.content.ComponentName;
import android.content.IClipboard;
import android.content.IOnPrimaryClipChangedListener;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Binder;
import android.os.IBinder;
import android.os.Process;
import android.os.RemoteCallbackList;
import android.os.RemoteException;
import android.provider.Settings;
import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;

/**
 * Android clipboard policy with Windows clipboard transport. Read access follows
 * AOSP ClipboardService: package ownership, configured IME/background permission
 * or actual input focus, effective AppOps mode, then device lock. Writes do not
 * require focus. A task's resumed state is not evidence of Windows input focus.
 */
public final class ClipboardService extends IClipboard.Default
        implements WindowsHost.Listener, AutoCloseable {
    private static final String WINDOWS_LABEL = "Windows";
    private static final int MAX_IMAGE_BYTES = 64 * 1024 * 1024;

    /** Instance-owned native boundary. Only isolated fixtures inject an implementation. */
    interface Host {
        boolean available();
        long[] foregroundProcess();
        int sessionLockState();
        int sequence();
        String text();
        String html();
        byte[] image();
        int write(String text, String html, byte[] image);
        int clear();
        void addListener(WindowsHost.Listener listener);
        void removeListener(WindowsHost.Listener listener);
    }

    private static final class WindowsClipboard implements Host {
        private boolean publisherRetained;
        public boolean available() { return WindowsHost.available(); }
        public long[] foregroundProcess() { return WindowsHost.foregroundProcess(); }
        public int sessionLockState() { return WindowsHost.sessionLockState(); }
        public int sequence() { return WindowsHost.clipboardSequence(); }
        public String text() { return WindowsHost.clipboardText(); }
        public String html() { return WindowsHost.clipboardHtml(); }
        public byte[] image() { return WindowsHost.clipboardImage(); }
        public int write(String text, String html, byte[] image) {
            return WindowsHost.setClipboardSequence(text, html, image);
        }
        public int clear() { return WindowsHost.clearClipboardSequence(); }
        public synchronized void addListener(WindowsHost.Listener listener) {
            if (publisherRetained) return;
            if (!WindowsHost.retainClipboardPublisher()) {
                throw new IllegalStateException("Windows clipboard publisher unavailable");
            }
            publisherRetained = true;
            try { WindowsHost.addListener(listener); }
            catch (Throwable failure) {
                publisherRetained = false;
                WindowsHost.removeListener(listener);
                WindowsHost.releaseClipboardPublisher();
                throw failure;
            }
        }
        public synchronized void removeListener(WindowsHost.Listener listener) {
            WindowsHost.removeListener(listener);
            if (publisherRetained) {
                publisherRetained = false;
                WindowsHost.releaseClipboardPublisher();
            }
        }
    }

    // Guarded by mListeners. Use the identity of its cookie to distinguish a
    // callback re-registered with the same Binder and attribution information.
    private final Map<IBinder, ListenerInfo> mListenerRegistrations = new HashMap<>();
    private final RemoteCallbackList<IOnPrimaryClipChangedListener> mListeners =
            new RemoteCallbackList<>() {
                @Override public void onCallbackDied(IOnPrimaryClipChangedListener listener, Object cookie) {
                    synchronized (this) {
                        IBinder binder = listener.asBinder();
                        if (mListenerRegistrations.get(binder) == cookie) mListenerRegistrations.remove(binder);
                    }
                }
            };
    private final ActivityTaskManagerService mAtm;
    private final PackageManagerService mPm;
    private final CallerIdentity mCallers;
    private final AppOpsService mAppOps;
    private final Host mHost;
    private final AutoCloseable mPackageChanges;
    private final AutoCloseable mPackageRemovals;
    private final Object mLifecycleLock = new Object();
    private final Object mClipboardLock = new Object();
    private boolean mStarted;  // mLifecycleLock
    private volatile boolean mClosed;
    // mClipboardLock also serializes native reads/writes, keeping sequence and provenance together.
    private ClipData mClip;
    private String mSource;
    private Uri mOwnedImage;
    private volatile ClipUris mClipUris;
    private int mSequence;
    private boolean mHaveSequence;

    public ClipboardService(ActivityTaskManagerService atm, PackageManagerService pm) {
        this(atm, pm, new WindowsClipboard());
    }

    ClipboardService(ActivityTaskManagerService atm, PackageManagerService pm, Host host) {
        mAtm = Objects.requireNonNull(atm);
        mPm = Objects.requireNonNull(pm);
        mCallers = new CallerIdentity(pm, atm);
        mAppOps = pm.appOps();
        mHost = Objects.requireNonNull(host);
        mPackageChanges = pm.onPackageChange(pkg -> packageLifetimeChanged(pkg.packageName));
        mPackageRemovals = pm.onPackageRemoval(this::packageLifetimeChanged);
    }

    /**
     * The activity manager's URI grant ledger, resolved where a clipboard URI is granted
     * or retired. A clip with content URIs is never published or released without it.
     */
    private UriGrants uriGrants() {
        return Objects.requireNonNull(mAtm.uriGrants(), "URI grants are not available");
    }

    /** The clipboard image provider, resolved where a clipboard image is stored or shared. */
    private ClipboardImageProvider images() {
        return Objects.requireNonNull(mAtm.clipboardImages(), "Clipboard images are not available");
    }

    private record ListenerInfo(int uid, String packageName, String attributionTag, ApkPackage owner) { }
    private record Callback(IOnPrimaryClipChangedListener listener, ListenerInfo identity) { }
    // The registered provider owns imported host images, so those have no app publisher.
    private static final class ClipUris {
        final ListenerInfo publisher;
        final IBinder owner = new Binder("Clipboard URI permissions");
        final List<Uri> uris;
        final Set<ApkPackage> readers = new HashSet<>();  // mClipboardLock
        volatile boolean retired;

        ClipUris(ListenerInfo publisher, List<Uri> uris) {
            this.publisher = publisher;
            this.uris = uris;
        }
    }

    private ListenerInfo caller(String packageName, String attributionTag) {
        if (mClosed) throw new IllegalStateException("Clipboard service is closed");
        int uid = Binder.getCallingUid();
        checkPackage(uid, packageName);
        // Keep the authenticated installation with this request. A process
        // finishing an older installation must not use the replacement's grants.
        ApkPackage installed = mPm.get(packageName);
        if (uid != Process.ROOT_UID && !mCallers.isSystem()) {
            String actual = mCallers.requirePackage(packageName);
            ApkPackage process = mCallers.packageCodeForPid(Binder.getCallingPid(), uid);
            if (process == null || !actual.equals(process.packageName)) installed = null;
            else if (actual.equals(packageName)) installed = process;
        }
        return new ListenerInfo(uid, packageName, attributionTag, installed);
    }

    private void checkPackage(int uid, String packageName) {
        if (packageName == null || mAppOps.checkPackage(uid, packageName) != AppOpsManager.MODE_ALLOWED) {
            throw new SecurityException("Clipboard package does not belong to calling UID");
        }
    }

    private boolean current(ListenerInfo identity) {
        if (mPm.isRemovalPending(identity.packageName)) return false;
        return identity.owner == null
                ? "android".equals(identity.packageName)
                    && (identity.uid == Process.ROOT_UID || identity.uid == Process.SYSTEM_UID)
                : mPm.get(identity.packageName) == identity.owner && identity.owner.uid() == identity.uid;
    }

    private boolean backgroundPermission(ListenerInfo identity) {
        // The built-in android package is not represented by an installed APK.
        // Keep its UID permission policy explicit; no arbitrary claimed package is privileged.
        if (identity.owner == null) {
            return mPm.permissions().checkUidPermission(identity.uid,
                    Manifest.permission.READ_CLIPBOARD_IN_BACKGROUND, 0) == PackageManager.PERMISSION_GRANTED;
        }
        return mPm.permissions().checkPermission(identity.packageName,
                Manifest.permission.READ_CLIPBOARD_IN_BACKGROUND) == PackageManager.PERMISSION_GRANTED;
    }

    private boolean defaultIme(String packageName) {
        String configured = SettingsProvider.secureString(Settings.Secure.DEFAULT_INPUT_METHOD);
        ComponentName ime = configured == null ? null : ComponentName.unflattenFromString(configured);
        return ime != null && packageName.equals(ime.getPackageName());
    }

    private boolean access(ListenerInfo identity, int op, boolean note) {
        checkPackage(identity.uid, identity.packageName);  // AOSP ownership check precedes privileges.
        if (!current(identity)) return false;
        if (op == AppOpsManager.OP_READ_CLIPBOARD && !backgroundPermission(identity)
                && !defaultIme(identity.packageName)
                && !mAtm.ownsForegroundProcess(identity.uid, mHost.foregroundProcess())) return false;
        // This is an internal system-server AppOps call for an already authenticated subject.
        // Do not run restricted-op inspection as the incoming application's Binder identity.
        long token = Binder.clearCallingIdentity();
        try {
            int mode = note ? mAppOps.noteOperation(op, identity.uid, identity.packageName,
                    identity.attributionTag, false, null, false).getOpMode()
                    : mAppOps.checkOperation(op, identity.uid, identity.packageName);
            return mode == AppOpsManager.MODE_ALLOWED;
        } finally { Binder.restoreCallingIdentity(token); }
    }

    private boolean readAllowed(String packageName, String tag, boolean note) {
        // With one Android user and no VirtualDeviceManager, AOSP maps requested user/device
        // IDs to the caller's user/default clipboard, rather than creating phantom silos.
        return access(caller(packageName, tag), AppOpsManager.OP_READ_CLIPBOARD, note)
                && mHost.sessionLockState() == 0;
    }

    private void start() {
        synchronized (mLifecycleLock) {
            if (mClosed) throw new IllegalStateException("Clipboard service is closed");
            if (mStarted) return;
            if (mHost.available()) mHost.addListener(this);
            mStarted = true;
        }
        syncFromWindows(false);
    }

    @Override public void onWindowsEvents(int events) {
        if (!mClosed && (events & WindowsHost.EVENT_CLIPBOARD) != 0) syncFromWindows(true);
    }

    /** Imports a stable native sequence. Callbacks run after releasing all service monitors. */
    private void syncFromWindows(boolean notify) {
        boolean changed = false;
        synchronized (mClipboardLock) {
            if (mClosed || !mHost.available()) return;
            for (int attempt = 0; attempt < 2; attempt++) {
                int sequence = mHost.sequence();
                if (sequence == 0 || (mHaveSequence && sequence == mSequence)) return;
                String text = mHost.text(), html = mHost.html();
                byte[] image = mHost.image();
                if (mHost.sequence() != sequence) continue;
                ClipData clip = null;
                Uri imageUri = null;
                ClipUris uris = null;
                if (image != null && image.length > 0 && image.length <= MAX_IMAGE_BYTES) {
                    List<String> mimeTypes = new ArrayList<>(3);
                    mimeTypes.add("image/bmp");
                    if (html != null) {
                        mimeTypes.add(ClipDescription.MIMETYPE_TEXT_HTML);
                        if (text == null) {
                            text = android.text.Html.fromHtml(html, android.text.Html.FROM_HTML_MODE_LEGACY).toString();
                        }
                    }
                    if (text != null) mimeTypes.add(ClipDescription.MIMETYPE_TEXT_PLAIN);
                    imageUri = images().add(image);
                    clip = new ClipData(new ClipDescription(WINDOWS_LABEL, mimeTypes.toArray(new String[0])),
                            new ClipData.Item(text, html, null, imageUri));
                    uris = new ClipUris(null, List.of(imageUri));
                }
                if (clip == null) {
                    if (text != null && html != null) clip = ClipData.newHtmlText(WINDOWS_LABEL, text, html);
                    else if (text != null) clip = ClipData.newPlainText(WINDOWS_LABEL, text);
                    else if (html != null) clip = ClipData.newHtmlText(WINDOWS_LABEL,
                            android.text.Html.fromHtml(html, android.text.Html.FROM_HTML_MODE_LEGACY).toString(), html);
                }
                mSequence = sequence; mHaveSequence = true;
                changed = replaceLocked(clip, null, imageUri, uris);
                break;
            }
        }
        if (changed && notify) notifyListeners();
    }

    private boolean replaceLocked(ClipData clip, String source, Uri image, ClipUris uris) {
        boolean changed = clip != null || mClip != null;
        if (mOwnedImage != null) images().remove(mOwnedImage);
        if (mClipUris != null) uriGrants().revokeByOwner(mClipUris.owner);
        mOwnedImage = image;
        mClipUris = uris;
        mClip = clip;
        mSource = clip == null ? null : source;
        if (clip != null) clip.getDescription().setTimestamp(System.currentTimeMillis());
        return changed;
    }

    private List<Callback> listenerSnapshot() {
        List<Callback> snapshot = new ArrayList<>();
        synchronized (mListeners) {
            if (mClosed) return snapshot;
            int count = mListeners.beginBroadcast();
            try {
                for (int i = 0; i < count; i++) {
                    snapshot.add(new Callback(mListeners.getBroadcastItem(i),
                            (ListenerInfo) mListeners.getBroadcastCookie(i)));
                }
            } finally { mListeners.finishBroadcast(); }
        }
        return snapshot;
    }

    private void packageLifetimeChanged(String packageName) {
        // Installation retirement can revoke the delegated capability while
        // readers are idle. Keep this callback independent of the clipboard
        // monitor and act only on the exact captured owner.
        ClipUris uris = mClipUris;
        if (uris != null && uris.publisher != null
                && uris.publisher.packageName.equals(packageName) && !current(uris.publisher)) {
            uris.retired = true;
            uriGrants().revokeByOwner(uris.owner);
        }
        pruneListeners();
    }

    private void pruneListeners() {
        synchronized (mListeners) {
            // Keep a concurrent re-registration from being removed for an older cookie.
            for (Callback callback : listenerSnapshot()) {
                if (!current(callback.identity)) unregisterLocked(callback.listener);
            }
        }
    }

    private boolean registered(Callback callback) {
        synchronized (mListeners) {
            return !mClosed && mListenerRegistrations.get(callback.listener.asBinder()) == callback.identity;
        }
    }

    private void unregisterLocked(IOnPrimaryClipChangedListener listener) {
        mListenerRegistrations.remove(listener.asBinder());
        mListeners.unregister(listener);
    }

    private void notifyListeners() {
        List<Callback> snapshot = listenerSnapshot();
        long token = Binder.clearCallingIdentity();
        try {
            for (Callback callback : snapshot) {
                if (mClosed) break;
                try {
                    // AOSP rechecks the saved UID/package/tag and notes a read for callbacks.
                    // Device lock gates getters, not the clipboard-change notification.
                    if (registered(callback) && current(callback.identity)
                            && access(callback.identity, AppOpsManager.OP_READ_CLIPBOARD, true)
                            && registered(callback) && current(callback.identity)) {
                        callback.listener.dispatchPrimaryClipChanged();
                    }
                } catch (RemoteException | RuntimeException failure) {
                    SystemServer.log("clipboard listener unavailable: " + failure.getClass().getSimpleName());
                }
            }
        } finally { Binder.restoreCallingIdentity(token); }
    }

    /** What Windows gets for {@code clip}: {text, html or null}. */
    private static String[] forWindows(ClipData clip) {
        StringBuilder text = new StringBuilder();
        StringBuilder html = new StringBuilder();
        boolean anyHtml = false;
        for (int i = 0; i < clip.getItemCount(); i++) {
            ClipData.Item item = clip.getItemAt(i);
            String t = null;
            if (item.getText() != null) {
                t = item.getText().toString();
            } else if (item.getHtmlText() != null) {
                t = android.text.Html.fromHtml(item.getHtmlText(),
                        android.text.Html.FROM_HTML_MODE_LEGACY).toString();
            } else if (item.getUri() != null) {
                t = item.getUri().toString();
            } else if (item.getIntent() != null) {
                t = item.getIntent().toUri(Intent.URI_INTENT_SCHEME);
            }
            if (t == null) continue;
            if (text.length() > 0) text.append("\r\n");
            text.append(t);
            if (item.getHtmlText() != null) {
                html.append(item.getHtmlText());
                anyHtml = true;
            } else {
                html.append(android.text.Html.escapeHtml(t));
            }
        }
        return new String[] {text.toString(), anyHtml ? html.toString() : null};
    }

    private static byte[] extractImage(ClipData clip) {
        for (int i = 0; i < clip.getItemCount(); i++) {
            android.net.Uri uri = clip.getItemAt(i).getUri();
            // A content URI is a provider capability, never a server-local filesystem path.
            if (uri == null || !"file".equals(uri.getScheme())
                    || (uri.getAuthority() != null && !uri.getAuthority().isEmpty()) || uri.getPath() == null) continue;
            File file = new File(uri.getPath());
            if (!file.isAbsolute() || !file.isFile() || file.length() <= 0 || file.length() > MAX_IMAGE_BYTES) continue;
            String name = file.getName().toLowerCase(java.util.Locale.ROOT);
            // The native bridge accepts BMP/DIB bytes; encoded PNG/JPEG/etc. are not DIB data.
            if (!name.endsWith(".bmp")) continue;
            try (java.io.InputStream input = new java.io.FileInputStream(file);
                    java.io.ByteArrayOutputStream output = new java.io.ByteArrayOutputStream()) {
                byte[] buffer = new byte[8192];
                int count;
                while ((count = input.read(buffer)) != -1) {
                    if (output.size() > MAX_IMAGE_BYTES - count) return null;
                    output.write(buffer, 0, count);
                }
                byte[] bytes = output.toByteArray();
                return bytes.length > 14 && bytes[0] == 'B' && bytes[1] == 'M' ? bytes : null;
            } catch (IOException ignored) { }
        }
        return null;
    }

    private static List<Uri> contentUris(ClipData clip) {
        List<Uri> uris = new ArrayList<>();
        for (int i = 0; i < clip.getItemCount(); i++) {
            ClipData.Item item = clip.getItemAt(i);
            addContentUri(uris, item.getUri());
            Intent intent = item.getIntent();
            if (intent != null) addContentUri(uris, intent.getData());
        }
        return uris;
    }

    private static void addContentUri(List<Uri> uris, Uri uri) {
        if (uri != null && "content".equals(uri.getScheme())) uris.add(uri);
    }

    private void enforceClipSourcePermission() {
        int uid = Binder.getCallingUid();
        if (uid == Process.ROOT_UID || uid == Process.SYSTEM_UID) return;
        if (mPm.permissions().checkProcessPermission(Manifest.permission.SET_CLIP_SOURCE,
                Binder.getCallingPid(), uid) == PackageManager.PERMISSION_GRANTED) return;
        throw new SecurityException("Clipboard source attribution requires SET_CLIP_SOURCE");
    }

    @Override public void setPrimaryClip(ClipData clip, String packageName, String tag, int userId, int deviceId) {
        setPrimaryClipInternal(clip, packageName, tag, packageName);
    }

    private void setPrimaryClipInternal(ClipData clip, String packageName, String tag, String source) {
        if (clip == null || clip.getItemCount() <= 0) throw new IllegalArgumentException("No items");
        ListenerInfo identity = caller(packageName, tag);
        if (!access(identity, AppOpsManager.OP_WRITE_CLIPBOARD, true)) return;
        ClipData saved = new ClipData(clip);
        List<Uri> uris = contentUris(saved);
        ClipUris clipUris = null;
        if (!uris.isEmpty()) {
            synchronized (mPm) {
                if (!current(identity)) return;
                // Validate the publisher before changing the clipboard. The
                // display attribution supplied to setPrimaryClipAsPackage is separate.
                uriGrants().prepareClipboard(identity.uid, null, uris);
            }
            clipUris = new ClipUris(identity, uris);
        }
        start();
        String[] windows = forWindows(saved);
        byte[] image = extractImage(saved);
        synchronized (mClipboardLock) {
            if (mClosed || !current(identity)) return;
            int writtenSequence = mHost.available() ? mHost.write(windows[0], windows[1], image) : 0;
            replaceLocked(saved, source, null, clipUris);
            if (writtenSequence != 0) { mSequence = writtenSequence; mHaveSequence = true; }
            else if (mHost.available()) {
                // No receipt can bind the saved typed clip to native data. The next
                // read/event must import the actual native generation, even if an
                // unsuccessful write left its sequence equal to our previous one.
                mHaveSequence = false;
            }
        }
        notifyListeners();
    }

    @Override public void setPrimaryClipAsPackage(ClipData clip, String packageName, String tag,
            int userId, int deviceId, String sourcePackage) {
        enforceClipSourcePermission();
        setPrimaryClipInternal(clip, packageName, tag, sourcePackage);
    }

    @Override public void clearPrimaryClip(String packageName, String tag, int userId, int deviceId) {
        ListenerInfo identity = caller(packageName, tag);
        if (!access(identity, AppOpsManager.OP_WRITE_CLIPBOARD, true)) return;
        start();
        boolean changed;
        synchronized (mClipboardLock) {
            if (mClosed || !current(identity)) return;
            int writtenSequence = mHost.available() ? mHost.clear() : 0;
            changed = replaceLocked(null, null, null, null);
            if (writtenSequence != 0) { mSequence = writtenSequence; mHaveSequence = true; }
            else if (mHost.available()) {
                // No receipt can bind the saved typed clip to native data. The next
                // read/event must import the actual native generation, even if an
                // unsuccessful write left its sequence equal to our previous one.
                mHaveSequence = false;
            }
        }
        if (changed) notifyListeners();
    }

    @Override public ClipData getPrimaryClip(String packageName, String tag, int userId, int deviceId) {
        ListenerInfo reader = caller(packageName, tag);
        if (!access(reader, AppOpsManager.OP_READ_CLIPBOARD, true) || mHost.sessionLockState() != 0) return null;
        start(); syncFromWindows(true);
        synchronized (mClipboardLock) {
            if (mClip == null || mClosed || !current(reader)) return null;
            ClipUris uris = mClipUris;
            if (uris == null) return new ClipData(mClip);
            // Typed Android contents belong to the native sequence acknowledged
            // when we wrote them. A failed write or external replacement cannot grant access.
            if (mHost.available() && (!mHaveSequence || mHost.sequence() != mSequence)) return null;
            synchronized (mPm) {
                if (!current(reader)) return null;
                try {
                    if (uris.retired || (uris.publisher != null && !current(uris.publisher))) {
                        throw new SecurityException("Retired clipboard publisher");
                    }
                    if (reader.owner != null && !uris.readers.contains(reader.owner)) {
                        if (mOwnedImage != null) {
                            images().grantRead(uris.owner, reader.packageName, mOwnedImage);
                        } else {
                            List<UriGrants.Grant> grants = uriGrants().prepareClipboard(uris.publisher.uid,
                                    reader.packageName, uris.uris);
                            uriGrants().publish(uris.owner, grants);
                        }
                        uris.readers.add(reader.owner);
                    }
                    return new ClipData(mClip);
                } catch (SecurityException revoked) {
                    // The old typed clip cannot be delivered. Leave any real native
                    // clipboard text in place until its owner changes it.
                    replaceLocked(null, null, null, null);
                }
            }
        }
        notifyListeners();
        return null;
    }

    @Override public ClipDescription getPrimaryClipDescription(String packageName, String tag, int userId, int deviceId) {
        if (!readAllowed(packageName, tag, false)) return null;
        start(); syncFromWindows(true);
        synchronized (mClipboardLock) { return mClip == null ? null : new ClipDescription(mClip.getDescription()); }
    }

    @Override public boolean hasPrimaryClip(String packageName, String tag, int userId, int deviceId) {
        if (!readAllowed(packageName, tag, false)) return false;
        start(); syncFromWindows(true);
        synchronized (mClipboardLock) { return mClip != null; }
    }

    @Override public boolean hasClipboardText(String packageName, String tag, int userId, int deviceId) {
        if (!readAllowed(packageName, tag, false)) return false;
        start(); syncFromWindows(true);
        synchronized (mClipboardLock) {
            if (mClip == null || mClip.getItemCount() == 0) return false;
            CharSequence text = mClip.getItemAt(0).getText();
            return text != null && text.length() > 0;
        }
    }

    @Override public String getPrimaryClipSource(String packageName, String tag, int userId, int deviceId) {
        enforceClipSourcePermission();
        if (!readAllowed(packageName, tag, false)) return null;
        start(); syncFromWindows(true);
        synchronized (mClipboardLock) { return mClip == null ? null : mSource; }
    }

    @Override public void addPrimaryClipChangedListener(IOnPrimaryClipChangedListener listener,
            String packageName, String tag, int userId, int deviceId) {
        ListenerInfo identity = caller(packageName, tag);
        start();
        synchronized (mListeners) {
            if (mClosed || !current(identity)) return;
            IBinder binder = listener.asBinder();
            if (mListeners.register(listener, identity)) mListenerRegistrations.put(binder, identity);
            else mListenerRegistrations.remove(binder);
        }
    }

    @Override public void removePrimaryClipChangedListener(IOnPrimaryClipChangedListener listener,
            String packageName, String tag, int userId, int deviceId) {
        caller(packageName, tag);
        synchronized (mListeners) { unregisterLocked(listener); }
    }

    @Override public boolean areClipboardAccessNotificationsEnabledForUser(int userId) { return false; }

    @Override public void close() {
        synchronized (mLifecycleLock) {
            if (mClosed) return;
            mClosed = true;
            if (mStarted) mHost.removeListener(this);
        }
        synchronized (mListeners) {
            mListenerRegistrations.clear();
            mListeners.kill();
        }
        try { mPackageChanges.close(); } catch (Exception failure) { throw new IllegalStateException(failure); }
        try { mPackageRemovals.close(); } catch (Exception failure) { throw new IllegalStateException(failure); }
        synchronized (mClipboardLock) { replaceLocked(null, null, null, null); }
    }
}
