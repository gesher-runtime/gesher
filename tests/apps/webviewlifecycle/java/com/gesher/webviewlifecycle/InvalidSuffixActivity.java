/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.webviewlifecycle;

import android.app.Activity;
import android.os.Bundle;
import android.webkit.WebView;

public final class InvalidSuffixActivity extends Activity {
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        Checks.report(this,"invalid-suffix",()->{
            Checks.require(LifecycleApplication.providerInitiallyNull && LifecycleApplication.setupFailure==null);
            String[] invalid={"../escape","x\\..\\escape","C:escape","bad/name","bad|name","bad?name","bad*name","bad\"name","bad<name","bad>name","..",".","dot.","space ","bad"+(char)0+"name",new String(new char[244]).replace((char)0,'x')};
            for(String value:invalid) {
                try { WebView.setDataDirectorySuffix(value); throw new AssertionError("Invalid suffix accepted"); }
                catch(IllegalArgumentException expected) { }
                Checks.require(Checks.provider()==null && Checks.suffix()==null);
            }
            WebView.setDataDirectorySuffix("safe_123");
            Checks.require("safe_123".equals(Checks.suffix()));
            Checks.initialized();
            Checks.configure(this);
            Checks.require(new java.io.File(getDataDir(),"app_webview_safe_123").isDirectory());
            System.out.println("viewlife: rejected-invalid="+invalid.length);
        });
    }
}
