/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.webviewlifecycle;

import android.app.Activity;
import android.os.Bundle;
import java.io.File;
import java.io.FileInputStream;
import java.lang.reflect.InvocationTargetException;
import java.nio.charset.StandardCharsets;

/** Different suffix text must not silently reuse a case-insensitive physical profile. */
public final class AliasActivity extends Activity {
    private static String marker(File file) throws Exception {
        byte[] bytes=new byte[(int)file.length()];
        try(FileInputStream input=new FileInputStream(file)) { Checks.require(input.read(bytes)==bytes.length); }
        return new String(bytes,StandardCharsets.UTF_8);
    }
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        Checks.report(this,"suffix-case-alias",()->{
            Checks.require("FIXTUREa".equals(Checks.suffix()));
            File alias=new File(getDataDir(),"app_webview_FIXTUREa");
            File owner=new File(alias,"lifecycle-owner.txt");
            if (!owner.exists()) {
                System.out.println("viewlife: case-alias filesystem-case-sensitive=true");
                return;
            }
            Checks.require("fixtureA".equals(marker(owner)));
            Checks.initialized();
            for(int attempt=0;attempt<2;attempt++) {
                Checks.require(!Checks.configured());
                try { Checks.configure(this); throw new AssertionError("Aliased suffix configured shared data"); }
                catch(InvocationTargetException expected) {
                    Throwable cause=expected.getCause();
                    Checks.require(cause instanceof IllegalArgumentException && cause.getMessage().contains("aliases"));
                }
                Checks.require(!Checks.configured() && "fixtureA".equals(marker(owner)));
            }
            System.out.println("viewlife: case-alias rejected retries=2 configured=false owner-intact=true");
        });
    }
}
