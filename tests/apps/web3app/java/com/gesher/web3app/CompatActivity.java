/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.web3app;

import android.app.Activity;
import android.net.Uri;
import android.os.Bundle;
import android.webkit.WebView;
import android.webkit.WebViewClient;

import androidx.webkit.ProxyConfig;
import androidx.webkit.ProxyController;
import androidx.webkit.ScriptHandler;
import androidx.webkit.WebMessageCompat;
import androidx.webkit.WebSettingsCompat;
import androidx.webkit.WebViewCompat;
import androidx.webkit.WebViewFeature;

import java.util.Collections;

/**
 * androidx.webkit on Gesher's WebView (ADR 0058), each step on a "Web3 compat" line:
 * WebViewFeature.isFeatureSupported, a ProxyController override (the page is on a host
 * only the test's proxy serves), a WebMessageListener (the page posts, the app replies
 * through the JavaScriptReplyProxy), a document-start script (and its removal),
 * WebViewCompat.postWebMessage, WebSettingsCompat's Safe Browsing and isMultiProcessEnabled.
 */
public class CompatActivity extends Activity {
    static final String[] FEATURES = {
        WebViewFeature.WEB_MESSAGE_LISTENER, WebViewFeature.DOCUMENT_START_SCRIPT,
        WebViewFeature.PROXY_OVERRIDE, WebViewFeature.MULTI_PROCESS,
        WebViewFeature.POST_WEB_MESSAGE, WebViewFeature.CREATE_WEB_MESSAGE_CHANNEL,
        WebViewFeature.WEB_MESSAGE_PORT_POST_MESSAGE, WebViewFeature.SAFE_BROWSING_ENABLE,
        WebViewFeature.VISUAL_STATE_CALLBACK, WebViewFeature.GET_WEB_VIEW_CLIENT,
    };

    private WebView mView;
    private ScriptHandler mEarly;
    private int mPage;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        StringBuilder f = new StringBuilder();
        boolean all = true;
        for (String feature : FEATURES) {
            boolean on = WebViewFeature.isFeatureSupported(feature);
            all &= on;
            f.append(' ').append(feature).append('=').append(on);
        }
        Web3.log("compat features:" + f);
        Web3.log("compat all features supported " + all);
        int proxy = Web3.port(0);
        // Before the first WebView: the override reaches WebView2's browser process.
        ProxyController.getInstance().setProxyOverride(new ProxyConfig.Builder()
                .addProxyRule("http://127.0.0.1:" + proxy).build(), Runnable::run,
                () -> Web3.log("compat proxy override set to 127.0.0.1:" + proxy));
        Web3.log("compat multi-process " + WebViewCompat.isMultiProcessEnabled());

        mView = new WebView(this);
        setContentView(mView);
        mView.getSettings().setJavaScriptEnabled(true);
        WebSettingsCompat.setSafeBrowsingEnabled(mView.getSettings(), false);
        Web3.log("compat safe browsing after setSafeBrowsingEnabled(false): "
                + WebSettingsCompat.getSafeBrowsingEnabled(mView.getSettings()));
        WebViewCompat.addWebMessageListener(mView, "web3", Collections.singleton("*"),
                (view, message, sourceOrigin, isMainFrame, reply) -> {
                    Web3.log("compat listener: '" + message.getData() + "' from " + sourceOrigin
                            + " main frame " + isMainFrame);
                    if (message.getData() != null && message.getData().startsWith("hello")) {
                        reply.postMessage("pong:" + message.getData());
                    }
                });
        mEarly = WebViewCompat.addDocumentStartJavaScript(mView,
                "window.__web3Early = 'early-' + location.host;", Collections.singleton("*"));
        mView.setWebViewClient(new WebViewClient() {
            @Override
            public void onPageFinished(WebView view, String url) {
                Web3.log("compat onPageFinished: " + url);
                mPage++;
                if (mPage == 1) firstPage(); else if (mPage == 2) secondPage();
            }
        });
        Web3.log("compat getWebViewClient is the app's "
                + (WebViewCompat.getWebViewClient(mView) != null));
        mView.loadUrl("http://web3-proxied.test/compat.html");
    }

    private void firstPage() {
        Web3.poll(mView, "document.title", v -> v.contains("got "), 30, title -> {
            Web3.log("compat title: " + title);
            WebViewCompat.postWebMessage(mView, new WebMessageCompat("compat-post"), Uri.parse("*"));
            Web3.poll(mView, "window.lastWindowMessage || ''", v -> v.contains("compat-post"), 20,
                    got -> {
                        Web3.log("compat page got postWebMessage: " + got);
                        mEarly.remove();
                        mView.reload();
                    });
        });
    }

    private void secondPage() {
        Web3.poll(mView, "document.title", v -> v.contains("got "), 30, title -> {
            Web3.log("compat after ScriptHandler.remove: " + title);
            Web3.log("compat done");
            finish();
        });
    }
}
