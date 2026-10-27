package com.gesher.opensl;

import android.app.Activity;
import android.os.Bundle;

/** The same real NDK contracts with manifest-allowed and denied recording. */
public final class OpenSlActivity extends Activity {
    private static native String run(boolean allowed);
    private String verdict = "not run";

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        try {
            System.loadLibrary("opensl_contracts");
            String report = run(!getPackageName().endsWith(".denied"));
            for (String line : report.split("\n")) System.out.println("OpenSlActivity: " + line);
            verdict = report.contains("media4 opensl: all passed\n") ? "all passed" : "FAILED";
        } catch (Throwable t) {
            verdict = "threw " + t;
            t.printStackTrace(System.out);
        }
        System.out.flush();
    }

    @Override
    protected void onResume() {
        super.onResume();
        System.out.println("OpenSlActivity onResume: " + verdict);
        System.out.flush();
    }
}
