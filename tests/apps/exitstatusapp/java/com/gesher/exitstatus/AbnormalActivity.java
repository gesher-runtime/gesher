package com.gesher.exitstatus;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.widget.TextView;

public final class AbnormalActivity extends Activity {
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        TextView text = new TextView(this);
        text.setText("Abnormal process exit");
        setContentView(text);
        System.out.println("Exec4 exit app: abnormal activity started");
        new Handler(getMainLooper()).postDelayed(() -> {
            System.out.println("Exec4 exit app: exiting with code 23");
            System.out.flush();
            System.exit(23);
        }, 500);
    }
}
