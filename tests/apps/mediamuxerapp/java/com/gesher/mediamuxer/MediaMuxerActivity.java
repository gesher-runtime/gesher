package com.gesher.mediamuxer;

import android.app.Activity;
import android.os.Bundle;

/** Loads the real ABI-specific NDK contract from the installed APK. */
public final class MediaMuxerActivity extends Activity {
    private static native String run();
    private String verdict = "not run";

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        try {
            System.loadLibrary("mediamuxer_test");
            String report = run();
            for (String line : report.split("\n")) System.out.println("MediaMuxerActivity: " + line);
            verdict = report.contains("media4 muxer: all passed\n") ? "all passed" : "FAILED";
        } catch (Throwable t) {
            verdict = "threw " + t;
            t.printStackTrace(System.out);
        }
        System.out.flush();
    }

    @Override
    protected void onResume() {
        super.onResume();
        System.out.println("MediaMuxerActivity onResume: " + verdict);
        System.out.flush();
    }
}
