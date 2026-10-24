/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.rtlapp;

import android.content.Context;
import android.util.AttributeSet;
import android.widget.EditText;

/** An EditText that reports where its cursor (selection) goes. */
public class SelectionEditText extends EditText {
    public SelectionEditText(Context context, AttributeSet attrs) {
        super(context, attrs);
    }

    @Override
    protected void onSelectionChanged(int start, int end) {
        super.onSelectionChanged(start, end);
        if (getText() != null && getText().length() > 0) {
            RtlActivity.say("RtlActivity selection " + start + "-" + end);
        }
    }
}
