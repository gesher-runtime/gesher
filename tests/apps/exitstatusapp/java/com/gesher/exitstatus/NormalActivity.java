package com.gesher.exitstatus;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.widget.TextView;

public final class NormalActivity extends Activity {
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        TextView text = new TextView(this);
        text.setText("Normal process exit");
        setContentView(text);
        System.out.println("Exec4 exit app: normal activity started");
        new Handler(getMainLooper()).postDelayed(this::finish, 500);
    }
}
