/* SPDX-License-Identifier: Apache-2.0 */
package com.gesher.restorefixture;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.os.SystemClock;
import android.util.Log;
import android.widget.TextView;
import java.io.File;

public final class ChildActivity extends Activity {
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        TextView view = new TextView(this);
        view.setText("Waiting for the task restoration fixture");
        setContentView(view);
        int original = getIntent().getIntExtra("parent", 0);
        File release = new File(getFilesDir(), "release-result");
        Handler handler = new Handler(Looper.getMainLooper());
        long end = SystemClock.elapsedRealtime() + 90000;
        Log.i(ParentActivity.TAG, "CHILD_READY pid=" + Process.myPid() + " parent=" + original
                + " task=" + getTaskId() + " release=" + release.getPath());
        handler.post(new Runnable() {
            @Override public void run() {
                if (release.exists() && release.delete()) {
                    Intent result = new Intent().putExtra("result", ParentActivity.RESULT).putExtra("parent", original);
                    setResult(RESULT_OK, result);
                    Log.i(ParentActivity.TAG, "CHILD_FINISH pid=" + Process.myPid());
                    finish();
                } else if (SystemClock.elapsedRealtime() > end) {
                    Log.e(ParentActivity.TAG, "FAIL controller did not release the child");
                    finish();
                } else {
                    handler.postDelayed(this, 100);
                }
            }
        });
    }
}
