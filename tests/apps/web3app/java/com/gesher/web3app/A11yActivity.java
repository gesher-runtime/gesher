/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.web3app;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.accessibility.AccessibilityManager;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityNodeProvider;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;

import java.util.ArrayList;
import java.util.List;

/**
 * Accessibility (ADR 0058), on "Web3 a11y" lines. First the WebView's
 * AccessibilityNodeProvider: the page's nodes (from WebView2's UI Automation tree), its
 * heading and its button, clicked through the provider. Then it waits for a UI Automation
 * client (54-web3.ps1, as Narrator would) to find the Android button, the edit text and the
 * check box in the window's UIA tree and to invoke, set and toggle them; the accessibility
 * state the app sees then, and an announcement.
 */
public class A11yActivity extends Activity {
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private int mClicks;
    private boolean mTextSet;
    private boolean mToggled;
    private WebView mWeb;
    private long mProviderUntil;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        Button button = new Button(this);
        button.setText("Android button");
        button.setOnClickListener(v -> {
            mClicks++;
            Web3.log("a11y Android button clicked " + mClicks);
        });
        EditText edit = new EditText(this);
        edit.setHint("Your name");
        edit.addTextChangedListener(new TextWatcher() {
            @Override
            public void beforeTextChanged(CharSequence s, int start, int count, int after) {
            }

            @Override
            public void onTextChanged(CharSequence s, int start, int before, int count) {
            }

            @Override
            public void afterTextChanged(Editable s) {
                Web3.log("a11y edit text: '" + s + "'");
                if (s.toString().equals("Narrator")) mTextSet = true;
            }
        });
        CheckBox check = new CheckBox(this);
        check.setText("Remember me");
        check.setOnCheckedChangeListener((v, on) -> {
            Web3.log("a11y check box checked " + on);
            mToggled = true;
        });
        mWeb = new WebView(this);
        mWeb.getSettings().setJavaScriptEnabled(true);
        layout.addView(button);
        layout.addView(edit);
        layout.addView(check);
        layout.addView(mWeb, new LinearLayout.LayoutParams(-1, 0, 1));
        setContentView(layout);
        mWeb.setWebViewClient(new WebViewClient() {
            @Override
            public void onPageFinished(WebView view, String url) {
                Web3.log("a11y onPageFinished: " + url);
                mHandler.postDelayed(A11yActivity.this::provider, 1500);
            }
        });
        mWeb.loadUrl("file:///android_asset/a11y.html");
    }

    private static void collect(AccessibilityNodeProvider p, int id, List<AccessibilityNodeInfo> out,
            int depth) {
        AccessibilityNodeInfo n = p.createAccessibilityNodeInfo(id);
        if (n == null || depth > 40 || out.size() > 500) return;
        out.add(n);
        for (int i = 0; i < n.getChildCount(); i++) {
            long child = n.getChildId(i);
            collect(p, AccessibilityNodeInfo.getVirtualDescendantId(child), out, depth + 1);
        }
    }

    /** The page through the WebView's AccessibilityNodeProvider. */
    private void provider() {
        if (mProviderUntil == 0) mProviderUntil = android.os.SystemClock.uptimeMillis() + 15_000;
        AccessibilityNodeProvider p = mWeb.getAccessibilityNodeProvider();
        if (p == null) {
            Web3.log("a11y provider: none");
            waitForUia();
            return;
        }
        List<AccessibilityNodeInfo> nodes = new ArrayList<>();
        AccessibilityNodeInfo host = p.createAccessibilityNodeInfo(AccessibilityNodeProvider.HOST_VIEW_ID);
        for (int i = 0; host != null && i < host.getChildCount(); i++) {
            collect(p, AccessibilityNodeInfo.getVirtualDescendantId(host.getChildId(i)), nodes, 0);
        }
        AccessibilityNodeInfo press = null, heading = null, field = null;
        for (AccessibilityNodeInfo n : nodes) {
            CharSequence t = n.getText();
            String s = t != null ? t.toString() : "";
            if (s.equals("Press me") && press == null) press = n;
            if (n.isHeading() && s.contains("Web content") && heading == null) heading = n;
            CharSequence hint = n.getHintText();
            if (n.isEditable() && hint != null && hint.toString().contains("Web field")) field = n;
        }
        if ((press == null || heading == null || field == null)
                && android.os.SystemClock.uptimeMillis() < mProviderUntil) {
            mHandler.postDelayed(this::provider, 100);
            return;
        }
        Web3.log("a11y provider: " + nodes.size() + " nodes; heading "
                + (heading != null ? "'" + heading.getText() + "'" : "none") + "; button "
                + (press != null ? "'" + press.getText() + "' " + press.getClassName()
                        + " clickable " + press.isClickable() : "none")
                + "; edit " + (field != null ? field.getClassName() : "none"));
        if (press == null) {
            waitForUia();
            return;
        }
        int id = AccessibilityNodeInfo.getVirtualDescendantId(press.getSourceNodeId());
        boolean clicked = p.performAction(id, AccessibilityNodeInfo.ACTION_CLICK, null);
        AccessibilityNodeInfo target = field;
        Web3.poll(mWeb, "document.title", v -> v.contains("pressed"), 15, title -> {
            Web3.log("a11y provider click " + clicked + ": title " + title);
            if (target != null) {
                Bundle args = new Bundle();
                args.putCharSequence(AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE,
                        "typed by accessibility");
                boolean set = p.performAction(AccessibilityNodeInfo.getVirtualDescendantId(
                        target.getSourceNodeId()), AccessibilityNodeInfo.ACTION_SET_TEXT, args);
                Web3.poll(mWeb, "document.getElementById('field').value", v -> v.contains("typed"),
                        15, value -> {
                            Web3.log("a11y provider set text " + set + ": field " + value);
                            waitForUia();
                        });
            } else {
                waitForUia();
            }
        });
    }

    private long mWaitUntil;

    /** Waits (up to 150 s) for the UI Automation client's invoke, set value and toggle. */
    private void waitForUia() {
        Web3.log("a11y ready for UI Automation");
        mWaitUntil = android.os.SystemClock.uptimeMillis() + 150_000;
        mHandler.post(this::check);
    }

    private boolean mAnnounced;

    private void check() {
        AccessibilityManager am = getSystemService(AccessibilityManager.class);
        if (am != null && am.isEnabled() && !mAnnounced) {
            mAnnounced = true;
            Web3.log("a11y AccessibilityManager enabled " + am.isEnabled());
            getWindow().getDecorView().announceForAccessibility("Web3 announcement");
        }
        boolean done = mClicks > 0 && mTextSet && mToggled;
        if (done || android.os.SystemClock.uptimeMillis() > mWaitUntil) {
            Web3.log("a11y UI Automation: clicks " + mClicks + ", text set " + mTextSet
                    + ", toggled " + mToggled + ", accessibility enabled "
                    + (am != null && am.isEnabled()));
            Web3.log("a11y done");
            finish();
            return;
        }
        mHandler.postDelayed(this::check, 250);
    }
}
