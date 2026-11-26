/* SPDX-License-Identifier: Apache-2.0 */
package com.gesher.restorefixture;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.util.Log;
import android.widget.TextView;

public final class ParentActivity extends Activity {
    static final String TAG = "F03Restore";
    static final String TEXT = "original instance input 528";
    static final String RESULT = "independent child result 764";
    private static final int REQUEST = 702;
    private TextView text;
    private int oldPid;
    private boolean restored;
    private boolean received;
    private boolean hierarchyRestored;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        text = new TextView(this);
        text.setId(0x1001);
        text.setFreezesText(true);
        text.setText("fresh view");
        setContentView(text);
        int launchPid = Process.myPid();
        int task = getTaskId();
        if (state != null) {
            oldPid = state.getInt("pid");
            restored = oldPid > 0 && launchPid != oldPid && task == state.getInt("task")
                    && TEXT.equals(state.getString("saved"));
            Log.i(TAG, "PARENT_RESTORED pid=" + launchPid + " old=" + oldPid + " task=" + task + " valid=" + restored);
            return;
        }
        if (getPreferences(MODE_PRIVATE).getBoolean("opened", false)) {
            Log.e(TAG, "FAIL real task was relaunched without the saved Bundle pid=" + launchPid);
            return;
        }
        getPreferences(MODE_PRIVATE).edit().putBoolean("opened", true).commit();
        text.setText(TEXT);
        Log.i(TAG, "PARENT_FIRST pid=" + launchPid + " task=" + task);
        new Handler(Looper.getMainLooper()).post(() -> startActivityForResult(
                new Intent(this, ChildActivity.class).putExtra("parent", launchPid), REQUEST));
    }

    @Override protected void onStop() {
        super.onStop();
        Log.i(TAG, "PARENT_STOPPED pid=" + Process.myPid());
    }

    @Override protected void onSaveInstanceState(Bundle out) {
        super.onSaveInstanceState(out);
        out.putString("saved", text.getText().toString());
        out.putInt("task", getTaskId());
        out.putInt("pid", Process.myPid());
        Log.i(TAG, "PARENT_SAVED pid=" + Process.myPid() + " task=" + getTaskId());
    }

    @Override protected void onRestoreInstanceState(Bundle state) {
        super.onRestoreInstanceState(state);
        hierarchyRestored = TEXT.contentEquals(text.getText());
        Log.i(TAG, "PARENT_VIEW valid=" + hierarchyRestored);
        announce();
    }

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        received = request == REQUEST && result == RESULT_OK && data != null
                && RESULT.equals(data.getStringExtra("result")) && oldPid == data.getIntExtra("parent", 0);
        Log.i(TAG, "PARENT_RESULT request=" + request + " valid=" + received);
        announce();
    }

    private void announce() {
        if (restored && received && hierarchyRestored) {
            Log.i(TAG, "PASS restored task and view received the child result exactly once");
            finish();
        }
    }
}
