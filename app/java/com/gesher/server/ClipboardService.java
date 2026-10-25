/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.ClipData;
import android.content.ClipDescription;
import android.content.IClipboard;
import android.content.IOnPrimaryClipChangedListener;
import android.content.Intent;
import android.os.RemoteCallbackList;
import android.os.RemoteException;

/**
 * {@code clipboard}: ClipboardService, one with Windows' clipboard (devsvc track,
 * docs/services.md "Device services", ADR 0038). An app's primary clip goes to Windows as
 * text (CF_UNICODETEXT: the items' text, HTML as text, URIs and intents as their strings,
 * one line each) and, when an item has HTML, as "HTML Format" too; what Windows' clipboard
 * gets from elsewhere (WM_CLIPBOARDUPDATE, through {@link WindowsHost}) becomes the primary
 * clip (HTML text if Windows has HTML, else plain text; nothing if it has no text), and
 * the apps' OnPrimaryClipChangedListeners are told either way. Every app may read it (no
 * focus or IME check, as Gesher grants what apps ask).
 */
final class ClipboardService extends IClipboard.Default implements WindowsHost.Listener {
    private static final String WINDOWS_LABEL = "Windows";

    private final RemoteCallbackList<IOnPrimaryClipChangedListener> mListeners =
            new RemoteCallbackList<>();
    // Guarded by this.
    private ClipData mClip;
    private String mSource;      // the package that set it, null for Windows
    private int mSequence = -1;  // Windows' clipboard sequence number mClip matches
    private boolean mStarted;

    /** Listens to Windows' clipboard from the first call on. */
    private void start() {
        synchronized (this) {
            if (mStarted) return;
            mStarted = true;
        }
        if (!WindowsHost.available()) return;
        WindowsHost.addListener(this);
        syncFromWindows(false);
        SystemServer.log("clipboard: linked with Windows' clipboard");
    }

    @Override
    public void onWindowsEvents(int events) {
        if ((events & WindowsHost.EVENT_CLIPBOARD) != 0) syncFromWindows(true);
    }

    /**
     * Takes Windows' clipboard as the primary clip if it changed since it was last seen (or
     * set from here); tells the listeners if {@code notify}.
     */
    private void syncFromWindows(boolean notify) {
        int seq = WindowsHost.clipboardSequence();
        synchronized (this) {
            if (seq == mSequence) return;  // ours, or seen
        }
        String text = WindowsHost.clipboardText();
        String html = WindowsHost.clipboardHtml();
        ClipData clip = null;
        if (text != null && html != null) {
            clip = ClipData.newHtmlText(WINDOWS_LABEL, text, html);
        } else if (text != null) {
            clip = ClipData.newPlainText(WINDOWS_LABEL, text);
        } else if (html != null) {
            clip = ClipData.newHtmlText(WINDOWS_LABEL, android.text.Html.fromHtml(html,
                    android.text.Html.FROM_HTML_MODE_LEGACY).toString(), html);
        }
        synchronized (this) {
            if (seq == mSequence) return;  // another thread took it meanwhile
            mSequence = seq;
            mClip = clip;
            mSource = null;
        }
        SystemServer.log("clipboard: from Windows (" + (clip == null ? "no text"
                : (text != null ? text.length() : 0) + " characters" + (html != null
                        ? ", HTML" : "")) + ")");
        if (notify) notifyListeners();
    }

    private void notifyListeners() {
        int n = mListeners.beginBroadcast();
        try {
            for (int i = 0; i < n; i++) {
                try {
                    mListeners.getBroadcastItem(i).dispatchPrimaryClipChanged();
                } catch (RemoteException e) {
                    // gone
                }
            }
        } finally {
            mListeners.finishBroadcast();
        }
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

    // --- IClipboard -------------------------------------------------------------------------

    @Override
    public void setPrimaryClip(ClipData clip, String callingPackage, String attributionTag,
            int userId, int deviceId) {
        start();
        if (clip == null || clip.getItemCount() <= 0) {
            throw new IllegalArgumentException("No items");
        }
        String[] windows = forWindows(clip);
        boolean written = WindowsHost.available() && WindowsHost.setClipboard(windows[0],
                windows[1]);
        synchronized (this) {
            mClip = clip;
            mSource = callingPackage;
            if (written) mSequence = WindowsHost.clipboardSequence();
        }
        SystemServer.log("clipboard: set by " + callingPackage + " (" + windows[0].length()
                + " characters" + (windows[1] != null ? ", HTML" : "") + ")"
                + (written ? ", on Windows' clipboard" : WindowsHost.available()
                        ? ", Windows' clipboard could not be opened" : ""));
        notifyListeners();
    }

    @Override
    public void setPrimaryClipAsPackage(ClipData clip, String callingPackage,
            String attributionTag, int userId, int deviceId, String sourcePackage) {
        setPrimaryClip(clip, sourcePackage, attributionTag, userId, deviceId);
    }

    @Override
    public void clearPrimaryClip(String callingPackage, String attributionTag, int userId,
            int deviceId) {
        start();
        boolean written = WindowsHost.available() && WindowsHost.setClipboard("", null);
        synchronized (this) {
            mClip = null;
            mSource = callingPackage;
            if (written) mSequence = WindowsHost.clipboardSequence();
        }
        SystemServer.log("clipboard: cleared by " + callingPackage);
        notifyListeners();
    }

    @Override
    public ClipData getPrimaryClip(String pkg, String attributionTag, int userId,
            int deviceId) {
        start();
        syncFromWindows(true);
        synchronized (this) {
            return mClip;
        }
    }

    @Override
    public ClipDescription getPrimaryClipDescription(String callingPackage,
            String attributionTag, int userId, int deviceId) {
        start();
        syncFromWindows(true);
        synchronized (this) {
            return mClip != null ? mClip.getDescription() : null;
        }
    }

    @Override
    public boolean hasPrimaryClip(String callingPackage, String attributionTag, int userId,
            int deviceId) {
        start();
        syncFromWindows(true);
        synchronized (this) {
            return mClip != null;
        }
    }

    @Override
    public boolean hasClipboardText(String callingPackage, String attributionTag, int userId,
            int deviceId) {
        start();
        syncFromWindows(true);
        synchronized (this) {
            if (mClip == null || mClip.getItemCount() == 0) return false;
            CharSequence text = mClip.getItemAt(0).getText();
            return text != null && text.length() > 0;
        }
    }

    @Override
    public String getPrimaryClipSource(String callingPackage, String attributionTag,
            int userId, int deviceId) {
        synchronized (this) {
            return mSource;
        }
    }

    @Override
    public void addPrimaryClipChangedListener(IOnPrimaryClipChangedListener listener,
            String callingPackage, String attributionTag, int userId, int deviceId) {
        start();
        mListeners.register(listener);
    }

    @Override
    public void removePrimaryClipChangedListener(IOnPrimaryClipChangedListener listener,
            String callingPackage, String attributionTag, int userId, int deviceId) {
        mListeners.unregister(listener);
    }

    @Override
    public boolean areClipboardAccessNotificationsEnabledForUser(int userId) {
        return false;
    }
}
