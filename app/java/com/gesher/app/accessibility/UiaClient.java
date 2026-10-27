/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app.accessibility;

import android.graphics.Rect;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * UI Automation as a client (ADR 0058): a window's UIA subtree (WebView2's, for a
 * WebView's AccessibilityNodeProvider) and actions on its elements. The natives are
 * compat/accessibility/uia_client.cc; they run UIA on a thread of their own.
 */
public final class UiaClient {
    private UiaClient() {
    }

    /** Element flags, as uia_client.cc's. */
    public static final int ENABLED = 1, FOCUSABLE = 2, FOCUSED = 4, CLICKABLE = 8,
            CHECKABLE = 16, CHECKED = 32, EDITABLE = 64, PASSWORD = 128, VISIBLE = 256,
            SCROLLABLE = 512, HEADING = 1024, SELECTED = 2048;
    /** Actions. */
    public static final int CLICK = 1, FOCUS = 2, SET_VALUE = 3, SCROLL_INTO_VIEW = 5;

    /** UIA_*ControlTypeId. */
    public static final int BUTTON = 50000, CHECK_BOX = 50002, COMBO_BOX = 50003, EDIT = 50004,
            HYPERLINK = 50005, IMAGE = 50006, LIST_ITEM = 50007, LIST = 50008, PROGRESS_BAR = 50012,
            RADIO_BUTTON = 50013, SLIDER = 50015, TEXT = 50020, DOCUMENT = 50030, GROUP = 50026,
            PANE = 50033, HEADER = 50034, TABLE = 50036;

    /** One element of a snapshot. */
    public static final class Element {
        public int id;
        public int parent;
        public int controlType;
        public String name = "";
        public String value = "";
        public String automationId = "";
        public String help = "";
        /** Screen coordinates. */
        public final Rect bounds = new Rect();
        public int flags;
        public final List<Integer> children = new ArrayList<>();

        public boolean has(int flag) {
            return (flags & flag) != 0;
        }
    }

    /** A window's subtree at one moment; its ids are valid for actions until the next. */
    public static final class Snapshot {
        public final int generation;
        public final List<Element> elements;
        public final List<Integer> top;

        Snapshot(int generation, List<Element> elements, List<Integer> top) {
            this.generation = generation;
            this.elements = Collections.unmodifiableList(elements);
            this.top = Collections.unmodifiableList(top);
        }

        public Element get(int id) {
            return id >= 0 && id < elements.size() ? elements.get(id) : null;
        }
    }

    private static native long nativeClientOrigin(long hwnd);

    private static native String nativeSnapshot(long hwnd, int maxNodes, int timeoutMs);

    private static native boolean nativeAct(int generation, int id, int action, String text);

    /** The client area's origin on the screen: {x, y}. */
    public static int[] clientOrigin(long hwnd) {
        if (!UiaBridge.load()) return new int[2];
        long v = nativeClientOrigin(hwnd);
        return new int[] {(int) (v >> 32), (int) v};
    }

    /** The subtree under a window (not the window itself), or null when UIA did not answer. */
    public static Snapshot snapshot(long hwnd, int maxNodes, int timeoutMs) {
        if (!UiaBridge.load() || hwnd == 0) return null;
        String text = nativeSnapshot(hwnd, maxNodes, timeoutMs);
        if (text == null) return null;
        int generation = 0;
        List<Element> all = new ArrayList<>();
        List<Integer> top = new ArrayList<>();
        for (String line : text.split("\n")) {
            String[] f = line.split("\t", -1);
            try {
                if (f[0].equals("gen")) {
                    generation = Integer.parseInt(f[1]);
                    continue;
                }
                if (f.length < 9) continue;
                Element e = new Element();
                e.id = Integer.parseInt(f[0]);
                e.parent = Integer.parseInt(f[1]);
                e.controlType = Integer.parseInt(f[2]);
                e.name = f[3];
                e.value = f[4];
                String[] b = f[5].split(" ");
                e.bounds.set(Integer.parseInt(b[0]), Integer.parseInt(b[1]),
                        Integer.parseInt(b[2]), Integer.parseInt(b[3]));
                e.flags = Integer.parseInt(f[6]);
                e.automationId = f[7];
                e.help = f[8];
                all.add(e);
            } catch (RuntimeException ex) {
                // a line this version does not read
            }
        }
        for (Element e : all) {
            if (e.parent < 0) {
                top.add(e.id);
            } else if (e.parent < all.size()) {
                all.get(e.parent).children.add(e.id);
            }
        }
        return new Snapshot(generation, all, top);
    }

    public static boolean act(Snapshot s, int id, int action, String text) {
        return UiaBridge.load() && s != null && nativeAct(s.generation, id, action, text);
    }
}
