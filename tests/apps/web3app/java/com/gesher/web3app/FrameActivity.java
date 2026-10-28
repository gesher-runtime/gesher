/* Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.web3app;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.webkit.WebView;
import androidx.webkit.JavaScriptReplyProxy;
import androidx.webkit.WebViewCompat;
import java.util.Collections;
import java.util.HashMap;
import java.util.Map;

/** Controlled frame and stale-reply fixture. All observations come from actual callbacks. */
public class FrameActivity extends Activity {
    private final Map<String, JavaScriptReplyProxy> replies = new HashMap<>();
    private WebView view;
    private boolean invalid;
    private boolean navigating;
    private String origin;
    private JavaScriptReplyProxy pendingNavigationReply;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        origin = "http://127.0.0.1:" + Web3.port(1);
        view = new WebView(this);
        setContentView(view);
        view.getSettings().setJavaScriptEnabled(true);
        WebViewCompat.addWebMessageListener(view, "web4", Collections.singleton(origin),
                (v, message, source, main, reply) -> {
                    String data = message.getData();
                    Web3.log("frame callback " + data + " origin=" + source + " main=" + main);
                    if (data == null || !data.endsWith("|true") || !origin.equals(source.toString())) {
                        invalid = true;
                        return;
                    }
                    String name = data.substring(0, data.indexOf('|'));
                    if (name.equals("during")) {
                        if (main || reply != replies.get("a")) invalid = true;
                        pendingNavigationReply = reply;
                        return;
                    }
                    if (!(name.equals("main") || name.equals("a") || name.equals("b")
                            || name.equals("nested") || name.equals("a2"))
                            || main != name.equals("main") || replies.containsKey(name)) {
                        invalid = true;
                        return;
                    }
                    for (JavaScriptReplyProxy other : replies.values()) {
                        if (other == reply) invalid = true;
                    }
                    replies.put(name, reply);
                    reply.postMessage("reply-" + name);
                    if (!navigating && replies.containsKey("main") && replies.containsKey("a")
                            && replies.containsKey("b") && replies.containsKey("nested")) {
                        navigating = true;
                        Web3.poll(view, "JSON.stringify(window.web4Seen)", s -> s.contains("a-reply:reply-a")
                                && s.contains("b-reply:reply-b") && s.contains("nested-reply:reply-nested")
                                && s.contains("bad-forge-sent") && s.contains("opaque-forge-sent"), 20,
                                s -> view.evaluateJavascript("(async()=>{let f=document.getElementById('a');"
                                        + "f.src='/web4-hold?name=a2';let held=false;for(let i=0;i<100;i++){"
                                        + "if(await (await fetch('/web4-hold-status')).text()==='held'){held=true;break;}"
                                        + "await new Promise(r=>setTimeout(r,100));}"
                                        + "if(held){try{f.contentWindow.web4.postMessage('during|true');"
                                        + "web4Seen.push('during-sent');}catch(e){web4Seen.push('during-error');}}"
                                        + "else web4Seen.push('hold-missing');await fetch('/web4-release');})()", null));
                    }
                    if (name.equals("a2")) {
                        replies.get("a").postMessage("STALE");
                        if (pendingNavigationReply != null) pendingNavigationReply.postMessage("RACE");
                        new Handler().postDelayed(this::check, 3000);
                    }
                });
        WebViewCompat.addDocumentStartJavaScript(view, "window.__web4Early=!!window.web4;",
                Collections.singleton(origin));
        view.loadUrl(origin + "/web4-main");
        new Handler().postDelayed(() -> {
            if (!isFinishing()) { Web3.log("frame timeout"); finish(); }
        }, 45000);
    }

    private void check() {
        view.evaluateJavascript("JSON.stringify(window.web4Seen)", value -> {
            Web3.log("frame pending callback observed " + (pendingNavigationReply != null));
            boolean observed = value != null && value.contains("a-reply:reply-a")
                    && value.contains("b-reply:reply-b") && value.contains("nested-reply:reply-nested")
                    && value.contains("a2-reply:reply-a2") && value.contains("bad-forge-attempt")
                    && value.contains("opaque-forge-attempt") && value.contains("bad-forge-sent")
                    && value.contains("opaque-forge-sent") && value.contains("during-sent")
                    && !value.contains("STALE") && !value.contains("RACE");
            Web3.log("frame observed " + value);
            Web3.log("frame controlled checks " + (observed && !invalid && replies.size() == 5));
            Web3.log("frame done");
            finish();
        });
    }
}
