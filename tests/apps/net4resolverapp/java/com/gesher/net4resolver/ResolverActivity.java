package com.gesher.net4resolver;
import android.app.Activity;
import android.os.Bundle;
public final class ResolverActivity extends Activity {
    private static native String run(boolean allowed);
    private String verdict = "FAILED";
    private boolean allowed;
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        allowed = !getPackageName().endsWith(".denied");
        try {
            System.loadLibrary("net4resolver");
            String report = run(allowed);
            for (String line : report.split("\n")) System.out.println("Net4ResolverActivity: " + line);
            if (report.contains("net4 resolver: all passed;")) verdict = "all passed";
        } catch (Throwable failure) { failure.printStackTrace(System.out); }
        System.out.flush();
    }
    @Override public void onResume() {
        super.onResume();
        System.out.println("Net4ResolverActivity onResume: " + verdict + "; allowed=" + (allowed ? 1 : 0));
        System.out.flush();
        finish();
    }
}
