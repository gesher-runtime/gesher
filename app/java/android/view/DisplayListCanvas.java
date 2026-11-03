/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.view;

import android.graphics.Canvas;
import android.graphics.RecordingCanvas;

/**
 * Compatibility stand-in for android.view.DisplayListCanvas (pre-API 29),
 * required for class verification in Jetpack Compose UI.
 */
public class DisplayListCanvas extends Canvas {
    public DisplayListCanvas() {
        super();
    }

    public DisplayListCanvas(RecordingCanvas canvas) {
        super();
    }

    public void drawRenderNode(RenderNode renderNode) {
    }
}
