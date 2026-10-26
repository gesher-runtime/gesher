/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.winapp;

import android.graphics.Color;
import android.os.Bundle;
import android.view.View;

/**
 * An Activity whose manifest asks for landscape (screenOrientation="landscape"): its task's
 * display turns (a 400x800 display gives it an 800x400 window). Blue, so the test sees its
 * window's pixels; reports its content's size.
 */
public class LandActivity extends Reporting {
    private String mLaidOut;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        final View blue = new View(this);
        blue.setBackgroundColor(Color.BLUE);
        setContentView(blue);
        blue.getViewTreeObserver().addOnGlobalLayoutListener(() -> {
            if (blue.getWidth() == 0) return;
            String where = "content " + blue.getWidth() + "x" + blue.getHeight();
            if (!where.equals(mLaidOut)) {
                mLaidOut = where;
                say("LandActivity laid out: " + where);
            }
        });
    }
}
