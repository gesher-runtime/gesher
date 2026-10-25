/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.lifeapp;

import android.app.AlertDialog;
import android.content.Intent;
import android.graphics.Color;
import android.os.Bundle;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.PopupWindow;
import android.widget.TextView;

/**
 * The lifecycle test app's launcher Activity (tests/windows/tracks/48-lifecycle.ps1): a red
 * View (clicks on it are reported) and buttons that start a translucent Activity, a
 * dialog-themed Activity and an opaque one, show a PopupWindow that is not focusable and
 * watches touches outside it (FLAG_WATCH_OUTSIDE_TOUCH: ACTION_OUTSIDE dismisses it), and
 * an AlertDialog. Reports where they are (centers, in the display's coordinates: the app
 * window's client area) each time that changes.
 */
public class LifeActivity extends Reporting {
    private String mLaidOut;

    private Button button(LinearLayout parent, String text, View.OnClickListener l) {
        Button b = new Button(this);
        b.setText(text);
        b.setAllCaps(false);
        b.setOnClickListener(l);
        parent.addView(b, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        return b;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        final View red = new View(this);
        red.setBackgroundColor(Color.RED);
        red.setOnClickListener(v -> say("LifeActivity red clicked"));
        root.addView(red, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                Math.round(160 * getResources().getDisplayMetrics().density)));
        final Button translucent = button(root, "Translucent activity",
                v -> startActivity(new Intent(this, TranslucentActivity.class)));
        final Button dialogActivity = button(root, "Dialog-themed activity",
                v -> startActivity(new Intent(this, DialogThemeActivity.class)));
        final Button opaque = button(root, "Opaque activity",
                v -> startActivity(new Intent(this, OpaqueActivity.class)));
        final Button popup = button(root, "Popup", v -> showPopup(v));
        final Button dialog = button(root, "Dialog", v -> showDialog());
        setContentView(root);
        root.getViewTreeObserver().addOnGlobalLayoutListener(() -> {
            if (root.getWidth() == 0) return;
            int[] at = new int[2];
            red.getLocationOnScreen(at);
            String where = "red " + at[0] + "," + at[1] + "," + (at[0] + red.getWidth()) + ","
                    + (at[1] + red.getHeight()) + ", translucent at " + center(translucent)
                    + ", dialog activity at " + center(dialogActivity) + ", opaque at "
                    + center(opaque) + ", popup at " + center(popup) + ", dialog at "
                    + center(dialog) + ", size " + root.getWidth() + "x" + root.getHeight();
            if (!where.equals(mLaidOut)) {
                mLaidOut = where;
                say("LifeActivity laid out: " + where);
            }
        });
    }

    /**
     * A PopupWindow at the bottom of the window that is not focusable (so not touch modal:
     * touches outside it reach the Activity) and outside-touchable: a touch outside it is
     * ACTION_OUTSIDE (FLAG_WATCH_OUTSIDE_TOUCH), which dismisses it.
     */
    private void showPopup(View anchor) {
        TextView text = new TextView(this);
        text.setText("A popup");
        text.setTextSize(20);
        text.setGravity(Gravity.CENTER);
        text.setBackgroundColor(Color.YELLOW);
        PopupWindow popup = new PopupWindow(text, 200, 64, false /* focusable */);
        popup.setOutsideTouchable(true);
        popup.setTouchInterceptor((v, e) -> {
            if (e.getActionMasked() == MotionEvent.ACTION_OUTSIDE) {
                say("LifeActivity popup: ACTION_OUTSIDE at " + Math.round(e.getX()) + ","
                        + Math.round(e.getY()));
            }
            return false;
        });
        popup.setOnDismissListener(() -> say("LifeActivity popup dismissed"));
        popup.showAtLocation(anchor.getRootView(), Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL,
                0, 16);
        text.post(() -> say("LifeActivity popup shown at " + center(text)));
    }

    private void showDialog() {
        final AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle("Gesher dialog")
                .setMessage("A dialog over the red view")
                .setPositiveButton("OK", (d, which) -> say("LifeActivity dialog: OK"))
                .create();
        dialog.setOnDismissListener(d -> say("LifeActivity dialog dismissed"));
        dialog.setOnShowListener(d -> {
            View decor = dialog.getWindow().getDecorView();
            decor.post(() -> say("LifeActivity dialog shown: dim "
                    + dialog.getWindow().getAttributes().dimAmount + ", at " + center(decor)));
        });
        dialog.show();
    }
}
