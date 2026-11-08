/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.webviewlifecycle;

import android.app.Activity;
import android.os.Bundle;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;

public abstract class SuffixActivity extends Activity {
    protected abstract String selectedSuffix();
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        String suffix=selectedSuffix();
        Checks.report(this,"suffix-"+(suffix==null?"default":suffix),()->{
            Checks.require(java.util.Objects.equals(suffix,Checks.suffix()));
            Checks.initialized();
            Checks.configure(this);
            String directory="app_webview"+(suffix==null?"":"_"+suffix);
            File folder=new File(getDataDir(),directory);
            Checks.require(folder.isDirectory() && folder.getCanonicalFile().getParentFile().equals(getDataDir().getCanonicalFile()));
            File marker=new File(folder,"lifecycle-owner.txt");
            String owner=suffix==null?"default":suffix;
            if (marker.exists()) {
                byte[] bytes=new byte[(int)marker.length()];
                try(FileInputStream input=new FileInputStream(marker)) { Checks.require(input.read(bytes)==bytes.length); }
                Checks.require(owner.equals(new String(bytes,StandardCharsets.UTF_8)));
            }
            try(FileOutputStream output=new FileOutputStream(marker)) { output.write(owner.getBytes(StandardCharsets.UTF_8)); }
            try { android.webkit.WebView.setDataDirectorySuffix("replacement"); throw new AssertionError("Suffix changed after use"); }
            catch(IllegalStateException expected) { }
            Checks.installAgain();
            Checks.disableAfterUse();
            System.out.println("viewlife: directory "+directory+" isolated=true");
        });
    }
}
