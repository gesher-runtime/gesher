/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.popupapp;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Menu;
import android.view.MenuItem;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewTreeObserver;
import android.view.WindowManagerGlobal;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.ListView;
import android.widget.Spinner;
import android.widget.Toast;


/**
 * The popups test app (tests/windows/tracks/36-popups.ps1): buttons that open an
 * AlertDialog, a Toast, the options menu (one item, in the overflow), a Spinner's dropdown,
 * an Activity with a dialog theme and a second Activity in the task. Reports on stdout where
 * things are (centers in the display's coordinates: the app window's client area, which the
 * test turns into a popup's), what was clicked or selected, focus and lifecycle.
 */
public class PopupActivity extends Activity {
    private static final String TAG = "PopupActivity";

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private String mLaidOut;

    static void say(String message) {
        System.out.println(message);
        System.out.flush();
        Log.i(TAG, message);
    }

    /** The center of {@code v} in the display's coordinates: "x,y". */
    static String center(View v) {
        int[] at = new int[2];
        v.getLocationOnScreen(at);
        return (at[0] + v.getWidth() / 2) + "," + (at[1] + v.getHeight() / 2);
    }

    private static String top(View v) {
        if (v == null) return "none";
        int[] at = new int[2];
        v.getLocationOnScreen(at);
        return at[0] + "," + at[1] + " " + v.getWidth() + "x" + v.getHeight()
                + (v.getParent() instanceof View ? " in " + v.getParent().getClass().getSimpleName()
                        + " padding top " + ((View) v.getParent()).getPaddingTop() : "");
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.main);
        final View content = findViewById(android.R.id.content);
        // Where the views are, each time that changes (the action bar's insets come in a
        // later layout pass than the first).
        content.getViewTreeObserver().addOnGlobalLayoutListener(() -> {
            if (content.getWidth() == 0) return;
            View red = findViewById(R.id.red);
            int[] at = new int[2];
            red.getLocationOnScreen(at);
            String where = "dialog at " + center(findViewById(R.id.dialog))
                    + ", toast at " + center(findViewById(R.id.toast))
                    + ", menu at " + center(findViewById(R.id.menu))
                    + ", spinner at " + center(findViewById(R.id.spinner))
                    + ", dialog activity at " + center(findViewById(R.id.dialog_activity))
                    + ", second at " + center(findViewById(R.id.second))
                    + ", red " + at[0] + "," + at[1] + "," + (at[0] + red.getWidth()) + ","
                    + (at[1] + red.getHeight()) + ", action bar "
                    + (getActionBar() != null && getActionBar().isShowing()
                            ? getActionBar().getHeight() : -1)
                    + ", content at " + top(content) + ", action bar container at "
                    + top(getWindow().getDecorView().findViewById(getResources().getIdentifier(
                            "action_bar_container", "id", "android")));
            if (!where.equals(mLaidOut)) {
                mLaidOut = where;
                say("PopupActivity laid out: " + where);
            }
        });
        ((Button) findViewById(R.id.dialog)).setOnClickListener(v -> showDialog());
        findViewById(R.id.toast).setOnClickListener(v -> {
            Toast.makeText(this, "Gesher toast", Toast.LENGTH_SHORT).show();
            say("PopupActivity toast requested");
        });
        findViewById(R.id.menu).setOnClickListener(v -> {
            openOptionsMenu();
            say("PopupActivity options menu requested");
            findList("menu item", 0, 50);
        });
        findViewById(R.id.dialog_activity).setOnClickListener(
                v -> startActivity(new Intent(this, DialogActivity.class)));
        findViewById(R.id.second).setOnClickListener(
                v -> startActivity(new Intent(this, SecondActivity.class)));
        Spinner spinner = findViewById(R.id.spinner);
        ArrayAdapter<CharSequence> adapter = new ArrayAdapter<CharSequence>(this,
                android.R.layout.simple_spinner_item,
                getResources().getTextArray(R.array.choices));
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        spinner.setAdapter(adapter);
        spinner.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                say("PopupActivity spinner: " + parent.getItemAtPosition(position));
            }

            @Override
            public void onNothingSelected(AdapterView<?> parent) {
            }
        });
        spinner.setOnTouchListener((v, e) -> {
            if (e.getActionMasked() == android.view.MotionEvent.ACTION_UP) {
                mHandler.postDelayed(() -> findList("dropdown item 1", 1, 50), 200);
            }
            return false;
        });
        say("PopupActivity onCreate");
    }

    private void showDialog() {
        final AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle("Gesher dialog")
                .setMessage("A dialog over its activity")
                .setPositiveButton("OK", (d, which) -> say("PopupActivity dialog: OK"))
                .setNegativeButton("Cancel", (d, which) -> say("PopupActivity dialog: Cancel"))
                .create();
        dialog.setOnCancelListener(d -> say("PopupActivity dialog: cancelled"));
        dialog.setOnDismissListener(d -> say("PopupActivity dialog: dismissed"));
        dialog.setOnShowListener(d -> {
            final Button ok = dialog.getButton(AlertDialog.BUTTON_POSITIVE);
            ok.getViewTreeObserver().addOnGlobalLayoutListener(
                    new ViewTreeObserver.OnGlobalLayoutListener() {
                        @Override
                        public void onGlobalLayout() {
                            if (ok.getWidth() == 0) return;
                            ok.getViewTreeObserver().removeOnGlobalLayoutListener(this);
                            ok.post(() -> say("PopupActivity dialog shown: OK at " + center(ok)
                                    + ", dim " + dialog.getWindow().getAttributes().dimAmount));
                        }
                    });
            ok.requestLayout();
        });
        dialog.show();
        say("PopupActivity dialog requested");
    }

    /**
     * Reports where item {@code index} of a list in another window (a menu's, a dropdown's)
     * is, once it is laid out there.
     */
    private void findList(String what, int index, int tries) {
        for (View root : WindowManagerGlobal.getInstance().getWindowViews()) {
            if (root == getWindow().getDecorView() || !root.isAttachedToWindow()) continue;
            ListView list = findList(root);
            if (list != null && list.getChildCount() > index
                    && list.getChildAt(index).getWidth() > 0) {
                say("PopupActivity " + what + " at " + center(list.getChildAt(index)));
                return;
            }
        }
        if (tries > 0) mHandler.postDelayed(() -> findList(what, index, tries - 1), 200);
    }

    private static ListView findList(View v) {
        if (v instanceof ListView) return (ListView) v;
        if (v instanceof ViewGroup) {
            ViewGroup g = (ViewGroup) v;
            for (int i = 0; i < g.getChildCount(); i++) {
                ListView l = findList(g.getChildAt(i));
                if (l != null) return l;
            }
        }
        return null;
    }

    @Override
    public boolean onCreateOptionsMenu(Menu menu) {
        getMenuInflater().inflate(R.menu.options, menu);
        return true;
    }

    @Override
    public boolean onOptionsItemSelected(MenuItem item) {
        say("PopupActivity menu: " + item.getTitle());
        return true;
    }

    @Override
    public boolean dispatchTouchEvent(android.view.MotionEvent e) {
        if (e.getActionMasked() != android.view.MotionEvent.ACTION_MOVE) {
            say("PopupActivity touch " + android.view.MotionEvent.actionToString(e.getAction())
                    + " at " + (int) e.getX() + "," + (int) e.getY());
        }
        return super.dispatchTouchEvent(e);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        say("PopupActivity focus " + hasFocus);
    }

    @Override
    protected void onResume() {
        super.onResume();
        say("PopupActivity onResume");
    }

    @Override
    protected void onPause() {
        super.onPause();
        say("PopupActivity onPause");
    }

    @Override
    protected void onStop() {
        super.onStop();
        say("PopupActivity onStop");
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        say("PopupActivity onDestroy");
    }
}
