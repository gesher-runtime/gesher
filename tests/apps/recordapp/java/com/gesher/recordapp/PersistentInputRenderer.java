/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.recordapp;

import android.graphics.Color;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES20;
import android.view.Surface;

/** The test's real EGL connection remains unchanged across encoder ownership changes. */
final class PersistentInputRenderer implements AutoCloseable {
    private EGLDisplay display = EGL14.EGL_NO_DISPLAY;
    private EGLContext context = EGL14.EGL_NO_CONTEXT;
    private EGLSurface window = EGL14.EGL_NO_SURFACE;
    private final int size;
    private boolean closed;

    private static void require(boolean value, String operation) {
        PersistentInputActivity.require(value, operation + " EGL error 0x" + Integer.toHexString(EGL14.eglGetError()));
    }
    PersistentInputRenderer(Surface surface, int size) {
        this.size = size;
        try {
            display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
            require(display != EGL14.EGL_NO_DISPLAY, "eglGetDisplay");
            int[] version = new int[2];
            require(EGL14.eglInitialize(display, version, 0, version, 1), "eglInitialize");
            EGLConfig[] configs = new EGLConfig[1];
            int[] count = new int[1];
            int[] attributes = {EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
                EGL14.EGL_SURFACE_TYPE, EGL14.EGL_WINDOW_BIT, EGL14.EGL_RED_SIZE, 8,
                EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8, EGL14.EGL_ALPHA_SIZE, 8,
                0x3142 /* EGL_RECORDABLE_ANDROID */, 1, EGL14.EGL_NONE};
            require(EGL14.eglChooseConfig(display, attributes, 0, configs, 0, 1, count, 0)
                    && count[0] == 1, "eglChooseConfig");
            context = EGL14.eglCreateContext(display, configs[0], EGL14.EGL_NO_CONTEXT,
                    new int[]{EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE}, 0);
            require(context != EGL14.EGL_NO_CONTEXT, "eglCreateContext");
            window = EGL14.eglCreateWindowSurface(display, configs[0], surface, new int[]{EGL14.EGL_NONE}, 0);
            require(window != EGL14.EGL_NO_SURFACE, "eglCreateWindowSurface");
            int[] value = new int[1];
            require(EGL14.eglQuerySurface(display, window, EGL14.EGL_WIDTH, value, 0) && value[0] == size,
                    "input Surface width");
            require(EGL14.eglQuerySurface(display, window, EGL14.EGL_HEIGHT, value, 0) && value[0] == size,
                    "input Surface height");
        } catch (Throwable failure) {
            try { close(); } catch (Throwable cleanup) { failure.addSuppressed(cleanup); }
            throw failure;
        }
    }
    void draw(int color) {
        PersistentInputActivity.require(!closed, "EGL producer already closed");
        require(EGL14.eglMakeCurrent(display, window, window, context), "eglMakeCurrent");
        try {
            GLES20.glViewport(0, 0, size, size);
            GLES20.glClearColor(Color.red(color) / 255f, Color.green(color) / 255f,
                    Color.blue(color) / 255f, 1);
            GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
            GLES20.glFinish();
            PersistentInputActivity.require(GLES20.glGetError() == GLES20.GL_NO_ERROR, "actual GLES rendering failed");
            require(EGL14.eglSwapBuffers(display, window), "eglSwapBuffers");
        } finally {
            // Binder transactions may arrive on different threads. Release
            // thread-current ownership, retaining the exact EGL context/window.
            require(EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE,
                    EGL14.EGL_NO_CONTEXT), "release current EGL context");
        }
    }
    @Override public void close() {
        if (closed) return;
        closed = true;
        boolean ok = true;
        if (display != EGL14.EGL_NO_DISPLAY) {
            if (window != EGL14.EGL_NO_SURFACE) ok &= EGL14.eglDestroySurface(display, window);
            if (context != EGL14.EGL_NO_CONTEXT) ok &= EGL14.eglDestroyContext(display, context);
            ok &= EGL14.eglTerminate(display);
        }
        window = EGL14.EGL_NO_SURFACE; context = EGL14.EGL_NO_CONTEXT; display = EGL14.EGL_NO_DISPLAY;
        PersistentInputActivity.require(ok, "EGL producer cleanup failed");
    }
}
