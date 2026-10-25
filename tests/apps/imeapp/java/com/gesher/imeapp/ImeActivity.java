/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.imeapp;

import android.app.Activity;
import android.os.Bundle;
import android.text.Editable;
import android.text.Layout;
import android.text.Selection;
import android.text.TextWatcher;
import android.util.Log;
import android.view.KeyEvent;
import android.view.View;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.InputMethodInfo;
import android.view.inputmethod.InputMethodManager;
import android.view.inputmethod.InputMethodSubtype;
import android.widget.EditText;

import java.util.List;

/**
 * gesher-app's text input test app (tests/windows/tracks/35-ime.ps1): editors whose Enter is
 * done, search, next and done, and a password field. Reports on stdout each editor's text
 * (other than ASCII as backslash-u escapes) with its composing region, its cursor in window
 * pixels, editor actions, focus changes, and what InputMethodManager answers once the
 * window has the focus (and again on F5).
 */
public class ImeActivity extends Activity {
    private static final String TAG = "ImeActivity";

    private EditText mEdit;

    private static void say(String message) {
        System.out.println(message);
        System.out.flush();
        Log.i(TAG, message);
    }

    private static String escape(CharSequence s) {
        StringBuilder out = new StringBuilder();
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c >= 0x20 && c < 0x7f) {
                out.append(c);
            } else {
                out.append(String.format("\\u%04x", (int) c));
            }
        }
        return out.toString();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.main);
        mEdit = findViewById(R.id.edit);
        watch(mEdit, "edit", true);
        watch(findViewById(R.id.search), "search", true);
        watch(findViewById(R.id.next), "next", false);
        watch(findViewById(R.id.last), "last", true);
        watch(findViewById(R.id.password), "password", true);
        mEdit.requestFocus();
        say("ImeActivity onCreate");
    }

    private void watch(final EditText edit, final String name, final boolean consumeAction) {
        edit.addTextChangedListener(new TextWatcher() {
            @Override
            public void beforeTextChanged(CharSequence s, int start, int count, int after) {
            }

            @Override
            public void onTextChanged(CharSequence s, int start, int before, int count) {
            }

            @Override
            public void afterTextChanged(Editable s) {
                int cs = BaseInputConnection.getComposingSpanStart(s);
                int ce = BaseInputConnection.getComposingSpanEnd(s);
                String composing = cs >= 0 && ce > cs
                        ? " composing " + cs + "-" + ce + " '" + escape(s.subSequence(cs, ce)) + "'"
                        : "";
                say("ImeActivity text " + name + ": " + escape(s.toString()) + composing);
                edit.post(() -> reportCursor(edit, name));
            }
        });
        edit.setOnEditorActionListener((v, actionId, event) -> {
            say("ImeActivity action " + name + ": " + actionId
                    + (event != null ? " (key " + event.getKeyCode() + ")" : ""));
            return consumeAction;
        });
        edit.setOnFocusChangeListener((v, hasFocus) -> {
            if (hasFocus) say("ImeActivity focus " + name);
        });
    }

    /** The cursor in window pixels: x, top-bottom. */
    private static void reportCursor(EditText edit, String name) {
        Layout layout = edit.getLayout();
        if (layout == null) return;
        int offset = Selection.getSelectionEnd(edit.getText());
        if (offset < 0) return;
        int line = layout.getLineForOffset(offset);
        int[] at = new int[2];
        edit.getLocationInWindow(at);
        int x = at[0] + edit.getTotalPaddingLeft() - edit.getScrollX()
                + Math.round(layout.getPrimaryHorizontal(offset));
        int top = at[1] + edit.getTotalPaddingTop() - edit.getScrollY() + layout.getLineTop(line);
        int bottom = at[1] + edit.getTotalPaddingTop() - edit.getScrollY()
                + layout.getLineBottom(line);
        say("ImeActivity cursor " + name + ": " + x + "," + top + "-" + bottom);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) mEdit.postDelayed(this::reportInputMethod, 1000);
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_F5) {
            reportInputMethod();
            return true;
        }
        return super.onKeyDown(keyCode, event);
    }

    /** What InputMethodManager answers about the focused editor and the input method. */
    private void reportInputMethod() {
        InputMethodManager imm = getSystemService(InputMethodManager.class);
        View focus = getCurrentFocus();
        StringBuilder s = new StringBuilder("ImeActivity imm:");
        s.append(" active ").append(focus != null && imm.isActive(focus));
        s.append(", accepting ").append(imm.isAcceptingText());
        List<InputMethodInfo> enabled = imm.getEnabledInputMethodList();
        s.append(", methods [");
        for (InputMethodInfo info : enabled) {
            s.append(info.getId()).append(" '").append(info.loadLabel(getPackageManager()))
                    .append("' ").append(imm.getEnabledInputMethodSubtypeList(info, true).size())
                    .append(" subtypes");
        }
        s.append("]");
        InputMethodInfo current = imm.getCurrentInputMethodInfo();
        s.append(", current ").append(current != null ? current.getId() : "none");
        InputMethodSubtype subtype = imm.getCurrentInputMethodSubtype();
        s.append(", subtype ").append(subtype != null ? subtype.getLanguageTag() : "none");
        if (focus != null) {
            s.append(", showSoftInput ").append(imm.showSoftInput(focus, 0));
            s.append(", hideSoftInputFromWindow ")
                    .append(imm.hideSoftInputFromWindow(focus.getWindowToken(), 0));
            imm.restartInput(focus);
            s.append(", restartInput: active ").append(imm.isActive(focus));
        }
        say(s.toString());
    }

    @Override
    protected void onResume() {
        super.onResume();
        say("ImeActivity onResume");
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        say("ImeActivity onDestroy");
    }
}
