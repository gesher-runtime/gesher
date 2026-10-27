/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.winapp;

import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.graphics.Color;
import android.graphics.Point;
import android.hardware.display.DisplayManager;
import android.os.Bundle;
import android.os.Handler;
import android.view.Display;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;

/** Real Application Display objects/listeners observed across rotations and task focus. */
public class DisplayActivity extends Reporting implements DisplayManager.DisplayListener {
    private final Handler handler = new Handler();
    private DisplayManager manager;
    private Display held;
    private String previous;
    private boolean top;
    private int callbacks;
    private final Runnable poll = new Runnable() {
        @Override public void run() {
            report();
            handler.postDelayed(this, 150);
        }
    };

    @Override protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        manager = getApplicationContext().getSystemService(DisplayManager.class);
        held = manager.getDisplay(Display.DEFAULT_DISPLAY);
        manager.registerDisplayListener(this, handler);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.rgb(32, 160, 96));
        Button turn = new Button(this);
        turn.setText("Turn current task");
        turn.setAllCaps(false);
        turn.setOnClickListener(v -> setRequestedOrientation(getDisplay().getRotation() == 0
                ? ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE : ActivityInfo.SCREEN_ORIENTATION_PORTRAIT));
        root.addView(turn, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 64));
        Button task = new Button(this);
        task.setText("New landscape task");
        task.setAllCaps(false);
        task.setOnClickListener(v -> startActivity(new Intent(this, DisplayLandActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_MULTIPLE_TASK)));
        root.addView(task, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 64));
        setContentView(root);
        root.getViewTreeObserver().addOnGlobalLayoutListener(() -> {
            if (root.getWidth() > 0) say("W4 " + name() + " buttons turn " + center(turn)
                    + " task " + center(task));
        });
        handler.post(poll);
    }

    private static String display(Display display) {
        Point size = new Point();
        Point real = new Point();
        display.getSize(size);
        display.getRealSize(real);
        return (display.getRotation() * 90) + " size " + size.x + "x" + size.y
                + " real " + real.x + "x" + real.y;
    }

    private void report() {
        if (manager == null || held == null) return;
        String now = "shared " + display(held) + " fresh "
                + display(manager.getDisplay(Display.DEFAULT_DISPLAY)) + " own "
                + display(getDisplay()) + " top " + top + " callbacks " + callbacks;
        if (!now.equals(previous)) {
            previous = now;
            say("W4 " + name() + " " + now);
        }
    }

    @Override public void onDisplayAdded(int id) { }
    @Override public void onDisplayRemoved(int id) { }
    @Override public void onDisplayChanged(int id) {
        if (id == Display.DEFAULT_DISPLAY) { callbacks++; report(); }
    }
    @Override public void onTopResumedActivityChanged(boolean value) {
        super.onTopResumedActivityChanged(value);
        top = value;
        report();
    }
    @Override protected void onDestroy() {
        handler.removeCallbacks(poll);
        if (manager != null) manager.unregisterDisplayListener(this);
        super.onDestroy();
    }
}
