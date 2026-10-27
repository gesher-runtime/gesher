package com.gesher.mediaformat;

import android.app.Activity;
import android.os.Bundle;

/** Loads the ABI-specific NDK library from the installed APK. */
public final class MediaFormatActivity extends Activity {
    private static native String run();
    private String verdict = "not run";

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        try {
            System.loadLibrary("mediaformat_test");
            String report = run();
            for (String line : report.split("\n")) System.out.println("MediaFormatActivity: " + line);
            verdict = report.contains("media4 format: all passed\n") ? "all passed" : "FAILED";
        } catch (Throwable t) {
            verdict = "threw " + t;
            t.printStackTrace(System.out);
        }
        System.out.flush();
    }

    @Override
    protected void onResume() {
        super.onResume();
        System.out.println("MediaFormatActivity onResume: " + verdict);
        System.out.flush();
    }
}
