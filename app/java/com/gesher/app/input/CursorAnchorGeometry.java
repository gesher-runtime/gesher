/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app.input;

import android.graphics.Matrix;
import android.graphics.RectF;
import android.view.inputmethod.CursorAnchorInfo;

/** Measured UTF-16 character bounds, including explicit missing measurements. */
public final class CursorAnchorGeometry {
    private CursorAnchorGeometry() {}

    public static int[] characters(CursorAnchorInfo info, int originX, int originY) {
        CharSequence text = info.getComposingText();
        int start = info.getComposingTextStart();
        if (text == null || start < 0 || text.length() > Integer.MAX_VALUE / 6
                || (long) start + text.length() > Integer.MAX_VALUE) return new int[0];
        int[] rectangles = new int[text.length() * 6];
        Matrix matrix = info.getMatrix();
        for (int i = 0; i < text.length(); i++) {
            int at = i * 6;
            rectangles[at] = -1;
            RectF bounds = info.getCharacterBounds(start + i);
            if (bounds == null) continue;
            RectF mapped = new RectF(bounds);
            if (matrix != null) matrix.mapRect(mapped);
            if (!Float.isFinite(mapped.left) || !Float.isFinite(mapped.top)
                    || !Float.isFinite(mapped.right) || !Float.isFinite(mapped.bottom)) continue;
            rectangles[at] = start + i;
            rectangles[at + 1] = (int) Math.floor(mapped.left) - originX;
            rectangles[at + 2] = (int) Math.floor(mapped.top) - originY;
            rectangles[at + 3] = (int) Math.ceil(mapped.right) - originX;
            rectangles[at + 4] = (int) Math.ceil(mapped.bottom) - originY;
            rectangles[at + 5] = (info.getCharacterBoundsFlags(start + i)
                    & CursorAnchorInfo.FLAG_HAS_INVISIBLE_REGION) != 0 ? 1 : 0;
        }
        return rectangles;
    }
}
