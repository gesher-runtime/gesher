/* Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.web3app;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.webkit.WebView;
import androidx.webkit.WebViewCompat;
import java.util.Collections;
import java.util.HashSet;
import java.util.Set;

/** Wildcard must install before first script in both inherited and opaque frames. */
public class WildcardActivity extends Activity {
    private final Set<String> received = new HashSet<>();
    private boolean invalid;
    private boolean scheduled;
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        String expectedOrigin = "http://127.0.0.1:" + Web3.port(1);
        WebView view = new WebView(this);
        setContentView(view);
        view.getSettings().setJavaScriptEnabled(true);
        WebViewCompat.addWebMessageListener(view, "wild", Collections.singleton("*"),
                (v, message, origin, main, reply) -> {
                    String data = message.getData();
                    Web3.log("wildcard callback " + data + " origin=" + origin + " main=" + main);
                    if (data == null || !(data.equals("main") || data.equals("inherited")
                            || data.equals("opaque")) || main != "main".equals(data)
                            || !received.add(data)) invalid = true;
                    // AndroidX documents the tuple for inherited origins. Keep the
                    // opaque representation observable until a reference oracle is run.
                    if (origin == null || ("opaque".equals(data)
                            ? expectedOrigin.equals(origin.toString())
                            : !expectedOrigin.equals(origin.toString()))) invalid = true;
                    if (received.size() == 3 && !scheduled) {
                        scheduled = true;
                        new Handler().postDelayed(() -> {
                            Web3.log("wildcard first-script checks " + !invalid);
                            Web3.log("wildcard done");
                            finish();
                        }, 2000);
                    }
                });
        view.loadUrl("http://127.0.0.1:" + Web3.port(1) + "/web4-wildcard");
        new Handler().postDelayed(() -> {
            if (!isFinishing()) { Web3.log("wildcard timeout " + received); finish(); }
        }, 45000);
    }
}
