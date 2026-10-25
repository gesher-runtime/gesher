/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.devsvc;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;

/**
 * The clipboard: puts HTML text on it (which Windows' clipboard gets), then waits for a
 * primary clip change whose text starts with "from windows" (the test sets Windows'
 * clipboard) and finishes; gives up after 90 s.
 */
public class ClipActivity extends Activity {
    private ClipboardManager mClipboard;
    private boolean mDone;

    private final ClipboardManager.OnPrimaryClipChangedListener mListener = () -> {
        ClipData clip = mClipboard.getPrimaryClip();
        CharSequence text = clip != null && clip.getItemCount() > 0
                ? clip.getItemAt(0).getText() : null;
        Report.line("clip: primary clip changed: \"" + text + "\"");
        if (text != null && text.toString().startsWith("from windows") && !mDone) {
            mDone = true;
            Report.line("clip: changed from Windows: " + text + " (label "
                    + clip.getDescription().getLabel() + ")");
            finish();
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mClipboard = getSystemService(ClipboardManager.class);
        mClipboard.addPrimaryClipChangedListener(mListener);
        mClipboard.setPrimaryClip(ClipData.newHtmlText("devsvc", "android text 57",
                "<b>android html 57</b>"));
        ClipData back = mClipboard.getPrimaryClip();
        Report.line("clip: set; reads back \"" + back.getItemAt(0).getText() + "\", html "
                + back.getItemAt(0).getHtmlText());
        new Handler(Looper.getMainLooper()).postDelayed(() -> {
            if (!mDone) {
                Report.line("clip: no change from Windows came");
                mDone = true;
                finish();
            }
        }, 90_000);
    }

    @Override
    protected void onDestroy() {
        mClipboard.removePrimaryClipChangedListener(mListener);
        super.onDestroy();
    }
}
