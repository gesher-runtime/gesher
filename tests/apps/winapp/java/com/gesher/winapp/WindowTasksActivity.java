/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.winapp;

import android.content.Intent;
import android.os.Bundle;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;

/** Ordinary SDK Activities in three manifest processes for the required desktop gate. */
public class WindowTasksActivity extends Reporting {
    private Button button(LinearLayout parent, String label, View.OnClickListener action) {
        Button button = new Button(this);
        button.setText(label); button.setAllCaps(false); button.setOnClickListener(action);
        parent.addView(button, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        return button;
    }

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        setTitle("Gesher app tasks " + name());
        say("WINDOW_TASK role=" + name() + " pid=" + android.os.Process.myPid()
                + " uid=" + android.os.Process.myUid() + " task=" + getTaskId()
                + " separate=" + "1".equals(System.getenv("GESHER_SEPARATE_WINDOWS")));
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        Button first = button(layout, "First remote task", view -> startActivity(
                new Intent(this, RemoteWindowActivity.class)
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_MULTIPLE_TASK)));
        Button second = button(layout, "Second remote task", view -> startActivity(
                new Intent(this, SecondRemoteWindowActivity.class)
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_MULTIPLE_TASK)));
        Button minimize = button(layout, "Minimize this task", view ->
                say(name() + " minimize=" + moveTaskToBack(true)));
        Button finish = button(layout, "Finish this task", view -> finishAndRemoveTask());
        setContentView(layout);
        layout.getViewTreeObserver().addOnGlobalLayoutListener(() -> {
            if (layout.getWidth() > 0) say("WINDOW_BUTTONS role=" + name() + " pid=" + android.os.Process.myPid() + " first=" + center(first)
                    + " second=" + center(second) + " minimize=" + center(minimize)
                    + " finish=" + center(finish));
        });
    }
}
