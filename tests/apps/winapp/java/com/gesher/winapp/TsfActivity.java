/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.winapp;

import android.os.Bundle;
import android.os.Handler;
import android.text.InputType;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.InputMethodManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;

/** A real focused EditText reconverted by the explicitly loaded Windows test service. */
public final class TsfActivity extends Reporting {
    private final Handler handler = new Handler();
    private EditText edit;
    private Button reconvert, password, number, normal;
    private int generation;

    @Override protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        edit = new EditText(this);
        root.addView(edit, new LinearLayout.LayoutParams(-1, 150));
        reconvert = button(root, "Reconvert selection", () -> reconvert(0, generation));
        password = button(root, "Password", () -> restrict("password", InputType.TYPE_CLASS_TEXT
                | InputType.TYPE_TEXT_VARIATION_PASSWORD));
        number = button(root, "Number", () -> restrict("number", InputType.TYPE_CLASS_NUMBER));
        normal = button(root, "Normal editor", this::normal);
        setContentView(root);
        root.post(() -> {
            say("W4 TSF buttons reconvert " + center(reconvert) + " password " + center(password)
                    + " number " + center(number) + " normal " + center(normal));
            normal();
        });
    }

    private Button button(LinearLayout root, String label, Runnable action) {
        Button button = new Button(this);
        button.setText(label);
        button.setFocusable(false);
        button.setOnClickListener(v -> action.run());
        root.addView(button, new LinearLayout.LayoutParams(-1, 85));
        return button;
    }

    private void normal() {
        generation++;
        edit.setInputType(InputType.TYPE_CLASS_TEXT);
        edit.setText("before \u05e9\u05dc\u05d5\u05dd");
        edit.setSelection(7, 11);
        edit.requestFocus();
        getSystemService(InputMethodManager.class).restartInput(edit);
        say("W4 TSF normal selected Hebrew 7-11 generation " + generation);
    }

    private int invokeService() throws ReflectiveOperationException {
        Class<?> type = Class.forName("com.gesher.app.input.WindowsInputMethod");
        Object ime = type.getMethod("get").invoke(null);
        return (Integer) type.getMethod("runTextServiceTest", android.os.IBinder.class)
                .invoke(ime, edit.getWindowToken());
    }

    private void restrict(String kind, int inputType) {
        int current = ++generation;
        edit.setInputType(inputType);
        edit.setText(kind.equals("number") ? "12345" : "secret");
        edit.setSelection(edit.length());
        edit.requestFocus();
        getSystemService(InputMethodManager.class).restartInput(edit);
        handler.postDelayed(() -> {
            if (current != generation) return;
            try {
                int result = invokeService();
                say("W4 TSF restricted " + kind + " HRESULT " + Integer.toHexString(result)
                        + " unchanged " + edit.getText().toString().equals(kind.equals("number") ? "12345" : "secret"));
            } catch (ReflectiveOperationException e) { say("W4 TSF failure " + e); }
        }, 750);
    }

    private void reconvert(int attempt, int current) {
        if (current != generation) return;
        try {
            int result = invokeService();
            if (result == 0x8000ffff && attempt < 50) {
                handler.postDelayed(() -> reconvert(attempt + 1, current), 100);
                return;
            }
            say("W4 TSF service HRESULT " + Integer.toHexString(result));
            observe(0, current);
        } catch (ReflectiveOperationException e) { say("W4 TSF failure " + e); }
    }

    private void observe(int attempt, int current) {
        if (current != generation) return;
        boolean changed = edit.getText().toString().equals("before \u65e5\u672c");
        boolean finished = BaseInputConnection.getComposingSpanStart(edit.getText()) == -1
                && BaseInputConnection.getComposingSpanEnd(edit.getText()) == -1;
        if ((!changed || !finished || edit.getSelectionStart() != 9 || edit.getSelectionEnd() != 9)
                && attempt < 70) {
            handler.postDelayed(() -> observe(attempt + 1, current), 100);
            return;
        }
        say("W4 TSF actual EditText Japanese " + changed + " composition ended " + finished
                + " selection " + edit.getSelectionStart() + "-" + edit.getSelectionEnd()
                + " generation " + generation);
    }
}
