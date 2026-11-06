/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.webviewlifecycle;

import android.app.Activity;
import android.os.Bundle;
import android.webkit.CookieManager;
import android.webkit.WebView;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;

public final class RaceActivity extends Activity {
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        Checks.report(this,"concurrent-use-disable",()->{
            Checks.require(LifecycleApplication.providerInitiallyNull && LifecycleApplication.setupFailure==null);
            CountDownLatch ready=new CountDownLatch(2),gate=new CountDownLatch(1);
            AtomicBoolean disabled=new AtomicBoolean(),used=new AtomicBoolean();
            AtomicReference<Throwable> failure=new AtomicReference<>();
            Thread disabler=new Thread(()->{
                ready.countDown();
                try { gate.await(); WebView.disableWebView(); disabled.set(true); }
                catch (IllegalStateException expected) { }
                catch (Throwable error) { failure.set(error); }
            });
            Thread user=new Thread(()->{
                ready.countDown();
                try { gate.await(); Checks.require(CookieManager.getInstance()!=null); used.set(true); }
                catch (IllegalStateException expected) { }
                catch (Throwable error) { failure.set(error); }
            });
            disabler.setDaemon(true); user.setDaemon(true);
            disabler.start(); user.start();
            Checks.require(ready.await(5,TimeUnit.SECONDS)); gate.countDown();
            disabler.join(5000); user.join(5000);
            Checks.require(!disabler.isAlive() && !user.isAlive() && failure.get()==null);
            Checks.require(disabled.get()!=used.get());
            Checks.installAgain();
            if (disabled.get()) {
                Checks.require(Checks.provider()==null);
                Checks.disabled(()->CookieManager.getInstance());
            } else {
                Checks.require(Checks.provider()!=null);
                Checks.disableAfterUse();
            }
            System.out.println("viewlife: race disabled="+disabled.get()+" used="+used.get());
        });
    }
}
