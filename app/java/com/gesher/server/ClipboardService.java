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
import android.os.Binder;
import android.os.Process;
import android.os.RemoteCallbackList;
import android.os.RemoteException;
import android.provider.Settings;
import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;

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

    private final RemoteCallbackList<IOnPrimaryClipChangedListener> mListeners =
            new RemoteCallbackList<>();
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
    private File mOwnedImage;
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
        mPackageChanges = pm.onPackageChange(ignored -> pruneListeners());
        mPackageRemovals = pm.onPackageRemoval(ignored -> pruneListeners());
    }

    private record ListenerInfo(int uid, String packageName, String attributionTag, ApkPackage owner) { }
    private record Callback(IOnPrimaryClipChangedListener listener, ListenerInfo identity) { }

    private ListenerInfo caller(String packageName, String attributionTag) {
        if (mClosed) throw new IllegalStateException("Clipboard service is closed");
        int uid = Binder.getCallingUid();
        checkPackage(uid, packageName);
        // The package/UID pair must also belong to this authenticated Binder process.
        // Root and the system package are native control-plane identities in AOSP.
        if (uid != Process.ROOT_UID) mCallers.requirePackage(packageName);
        return new ListenerInfo(uid, packageName, attributionTag, mPm.get(packageName));
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
                File imageFile = null;
                if (image != null && image.length > 0 && image.length <= MAX_IMAGE_BYTES) {
                    try {
                        File directory = new File(System.getProperty("java.io.tmpdir", "."), "gesher_clipboard");
                        if (!directory.isDirectory() && !directory.mkdirs()) throw new IOException("No clipboard cache");
                        imageFile = File.createTempFile("clip-", ".bmp", directory);
                        java.nio.file.Files.write(imageFile.toPath(), image);
                        ClipDescription description = new ClipDescription(WINDOWS_LABEL, new String[]{"image/bmp", "image/*"});
                        clip = new ClipData(description, new ClipData.Item(text, html, null,
                                android.net.Uri.fromFile(imageFile)));
                    } catch (IOException failure) {
                        deleteImage(imageFile); imageFile = null;
                        SystemServer.log("clipboard: could not cache native image: " + failure);
                    }
                }
                if (clip == null) {
                    if (text != null && html != null) clip = ClipData.newHtmlText(WINDOWS_LABEL, text, html);
                    else if (text != null) clip = ClipData.newPlainText(WINDOWS_LABEL, text);
                    else if (html != null) clip = ClipData.newHtmlText(WINDOWS_LABEL,
                            android.text.Html.fromHtml(html, android.text.Html.FROM_HTML_MODE_LEGACY).toString(), html);
                }
                mSequence = sequence; mHaveSequence = true;
                changed = replaceLocked(clip, null, imageFile);
                break;
            }
        }
        if (changed && notify) notifyListeners();
    }

    private static void deleteImage(File file) {
        if (file == null) return;
        try { java.nio.file.Files.deleteIfExists(file.toPath()); }
        catch (IOException failure) { SystemServer.log("clipboard: could not release cached image: " + failure); }
    }

    private boolean replaceLocked(ClipData clip, String source, File imageFile) {
        boolean changed = clip != null || mClip != null;
        deleteImage(mOwnedImage);
        mOwnedImage = imageFile;
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

    private void pruneListeners() {
        synchronized (mListeners) {
            // Keep a concurrent re-registration from being removed for an older cookie.
            for (Callback callback : listenerSnapshot()) {
                if (!current(callback.identity)) mListeners.unregister(callback.listener);
            }
        }
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
                    if (current(callback.identity) && access(callback.identity,
                            AppOpsManager.OP_READ_CLIPBOARD, true)) {
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
        if (!access(caller(packageName, tag), AppOpsManager.OP_WRITE_CLIPBOARD, true)) return;
        start();
        ClipData saved = new ClipData(clip);
        String[] windows = forWindows(saved);
        byte[] image = extractImage(saved);
        synchronized (mClipboardLock) {
            if (mClosed) return;
            int writtenSequence = mHost.available() ? mHost.write(windows[0], windows[1], image) : 0;
            replaceLocked(saved, source, null);
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
        if (!access(caller(packageName, tag), AppOpsManager.OP_WRITE_CLIPBOARD, true)) return;
        start();
        boolean changed;
        synchronized (mClipboardLock) {
            if (mClosed) return;
            int writtenSequence = mHost.available() ? mHost.clear() : 0;
            changed = replaceLocked(null, null, null);
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
        if (!readAllowed(packageName, tag, true)) return null;
        start(); syncFromWindows(true);
        synchronized (mClipboardLock) { return mClip == null ? null : new ClipData(mClip); }
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
            if (!mClosed && current(identity)) mListeners.register(listener, identity);
        }
    }

    @Override public void removePrimaryClipChangedListener(IOnPrimaryClipChangedListener listener,
            String packageName, String tag, int userId, int deviceId) {
        caller(packageName, tag);
        mListeners.unregister(listener);
    }

    @Override public boolean areClipboardAccessNotificationsEnabledForUser(int userId) { return false; }

    @Override public void close() {
        synchronized (mLifecycleLock) {
            if (mClosed) return;
            mClosed = true;
            if (mStarted) mHost.removeListener(this);
        }
        synchronized (mListeners) { mListeners.kill(); }
        try { mPackageChanges.close(); } catch (Exception failure) { throw new IllegalStateException(failure); }
        try { mPackageRemovals.close(); } catch (Exception failure) { throw new IllegalStateException(failure); }
        synchronized (mClipboardLock) { replaceLocked(null, null, null); }
    }
}
