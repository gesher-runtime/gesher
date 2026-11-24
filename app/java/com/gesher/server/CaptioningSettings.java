/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.provider.Settings;
import android.view.accessibility.CaptioningManager.CaptionStyle;

import java.util.LinkedHashMap;
import java.util.Map;

/**
 * Windows' caption style (Windows.Media.ClosedCaptioning.ClosedCaptionProperties, as
 * WindowsCaptions reads it) as the Settings.Secure keys that
 * android.view.accessibility.CaptioningManager reads in android-16.0.0_r4: the preset,
 * the custom style's colors, edge and typeface, and the font scale.
 *
 * <p>Every Windows value of "Default" is a value Windows leaves to the player; Android's
 * equivalent is an unspecified field of the custom style (CaptionStyle.COLOR_UNSPECIFIED,
 * EDGE_TYPE_UNSPECIFIED, no typeface), so a player keeps its own default for that field.
 * When every style field is Default, no style key is set at all and CaptioningManager
 * answers its own default preset, as on a device whose user never chose a style.
 *
 * <p>Not from Windows, so never written here: ACCESSIBILITY_CAPTIONING_ENABLED (Windows has
 * no "show captions" preference; caption styles apply to captions a player shows),
 * ACCESSIBILITY_CAPTIONING_LOCALE (no caption language preference) and the system audio
 * captioning keys (ODI_CAPTIONS_*).
 */
final class CaptioningSettings {
    /** The keys this mapping owns: each is set to its value or removed (null). */
    static final String[] KEYS = {
        Settings.Secure.ACCESSIBILITY_CAPTIONING_PRESET,
        Settings.Secure.ACCESSIBILITY_CAPTIONING_FOREGROUND_COLOR,
        Settings.Secure.ACCESSIBILITY_CAPTIONING_BACKGROUND_COLOR,
        Settings.Secure.ACCESSIBILITY_CAPTIONING_WINDOW_COLOR,
        Settings.Secure.ACCESSIBILITY_CAPTIONING_EDGE_TYPE,
        Settings.Secure.ACCESSIBILITY_CAPTIONING_EDGE_COLOR,
        Settings.Secure.ACCESSIBILITY_CAPTIONING_TYPEFACE,
        Settings.Secure.ACCESSIBILITY_CAPTIONING_FONT_SCALE,
    };

    // Indexes of WindowsCaptions' values (compat/devices/caption_win.h Properties).
    static final int FONT_COLOR = 0, COMPUTED_FONT_COLOR = 1, FONT_OPACITY = 2, FONT_SIZE = 3,
            FONT_STYLE = 4, FONT_EFFECT = 5, BACKGROUND_COLOR = 6, COMPUTED_BACKGROUND_COLOR = 7,
            BACKGROUND_OPACITY = 8, REGION_COLOR = 9, COMPUTED_REGION_COLOR = 10,
            REGION_OPACITY = 11, VALUES = 12;

    /** A fully transparent color that CaptionStyle.hasColor accepts (not COLOR_UNSPECIFIED). */
    static final int TRANSPARENT = 0x00000000;

    /** Windows' "Default" in each of its caption enums. */
    static final int DEFAULT = 0;

    private CaptioningSettings() {
    }

    /**
     * The Secure values for Windows' caption properties: key -> value, or key -> null to
     * remove it. Null when {@code windows} is not a complete, known answer (then nothing is
     * changed).
     */
    static Map<String, String> secureValues(int[] windows) {
        if (windows == null || windows.length != VALUES) return null;
        Integer foreground = color(windows[FONT_COLOR], windows[FONT_OPACITY], windows[COMPUTED_FONT_COLOR]);
        Integer background = color(windows[BACKGROUND_COLOR], windows[BACKGROUND_OPACITY],
                windows[COMPUTED_BACKGROUND_COLOR]);
        Integer window = color(windows[REGION_COLOR], windows[REGION_OPACITY], windows[COMPUTED_REGION_COLOR]);
        Integer edge = edgeType(windows[FONT_EFFECT]);
        String typeface = typeface(windows[FONT_STYLE]);
        Float scale = fontScale(windows[FONT_SIZE]);
        if (foreground == null || background == null || window == null || edge == null
                || typeface == null || scale == null) {
            return null;  // a value this mapping does not know: a newer Windows
        }
        Map<String, String> values = new LinkedHashMap<>();
        boolean custom = foreground != CaptionStyle.COLOR_UNSPECIFIED
                || background != CaptionStyle.COLOR_UNSPECIFIED
                || window != CaptionStyle.COLOR_UNSPECIFIED
                || edge != CaptionStyle.EDGE_TYPE_UNSPECIFIED || !typeface.isEmpty();
        values.put(Settings.Secure.ACCESSIBILITY_CAPTIONING_PRESET,
                custom ? Integer.toString(CaptionStyle.PRESET_CUSTOM) : null);
        values.put(Settings.Secure.ACCESSIBILITY_CAPTIONING_FOREGROUND_COLOR, custom ? Integer.toString(foreground) : null);
        values.put(Settings.Secure.ACCESSIBILITY_CAPTIONING_BACKGROUND_COLOR, custom ? Integer.toString(background) : null);
        values.put(Settings.Secure.ACCESSIBILITY_CAPTIONING_WINDOW_COLOR, custom ? Integer.toString(window) : null);
        values.put(Settings.Secure.ACCESSIBILITY_CAPTIONING_EDGE_TYPE, custom ? Integer.toString(edge) : null);
        // Windows has no edge color: unspecified (the player's).
        values.put(Settings.Secure.ACCESSIBILITY_CAPTIONING_EDGE_COLOR,
                custom ? Integer.toString(CaptionStyle.COLOR_UNSPECIFIED) : null);
        values.put(Settings.Secure.ACCESSIBILITY_CAPTIONING_TYPEFACE,
                custom && !typeface.isEmpty() ? typeface : null);
        values.put(Settings.Secure.ACCESSIBILITY_CAPTIONING_FONT_SCALE,
                scale.isNaN() ? null : Float.toString(scale));
        return values;
    }

    /**
     * ARGB of a Windows color and opacity: the named color (ClosedCaptionColor), or
     * Windows' computed color when the name is Default; the opacity's alpha, or the computed
     * color's when it is Default. Both Default: unspecified. Null: an unknown value.
     */
    static Integer color(int name, int opacity, int computed) {
        if (name == DEFAULT && opacity == DEFAULT) return CaptionStyle.COLOR_UNSPECIFIED;
        int rgb;
        switch (name) {
            case DEFAULT: rgb = computed & 0xFFFFFF; break;
            case 1: rgb = 0xFFFFFF; break;  // White
            case 2: rgb = 0x000000; break;  // Black
            case 3: rgb = 0xFF0000; break;  // Red
            case 4: rgb = 0x00FF00; break;  // Green
            case 5: rgb = 0x0000FF; break;  // Blue
            case 6: rgb = 0xFFFF00; break;  // Yellow
            case 7: rgb = 0xFF00FF; break;  // Magenta
            case 8: rgb = 0x00FFFF; break;  // Cyan
            default: return null;
        }
        int alpha;
        switch (opacity) {
            case DEFAULT: alpha = computed >>> 24; break;
            case 1: alpha = 0xFF; break;  // OneHundredPercent
            case 2: alpha = 0xBF; break;  // SeventyFivePercent
            case 3: alpha = 0x40; break;  // TwentyFivePercent
            case 4: alpha = 0x00; break;  // ZeroPercent
            default: return null;
        }
        // CaptionStyle.hasColor(c) is (c >>> 24) != 0 || (c & 0x00FFFF00) == 0: a fully
        // transparent color whose red or green is set (white at 0% is 0x00FFFFFF, which is
        // COLOR_UNSPECIFIED itself) reads as "no color", and the player would draw its own
        // opaque default. Transparent is the same whatever its RGB, so encode it as
        // 0x00000000, a color hasColor accepts.
        if (alpha == 0) return TRANSPARENT;
        return (alpha << 24) | rgb;
    }

    /** ClosedCaptionEdgeEffect as CaptionStyle's edge type; null when unknown. */
    static Integer edgeType(int effect) {
        switch (effect) {
            case DEFAULT: return CaptionStyle.EDGE_TYPE_UNSPECIFIED;
            case 1: return CaptionStyle.EDGE_TYPE_NONE;         // None
            case 2: return CaptionStyle.EDGE_TYPE_RAISED;       // Raised
            case 3: return CaptionStyle.EDGE_TYPE_DEPRESSED;    // Depressed
            case 4: return CaptionStyle.EDGE_TYPE_OUTLINE;      // Uniform
            case 5: return CaptionStyle.EDGE_TYPE_DROP_SHADOW;  // DropShadow
            default: return null;
        }
    }

    /**
     * ClosedCaptionStyle as a typeface name of Android's caption settings
     * (Settings' captioning_typeface_selector_values); "" for Default, null when unknown.
     */
    static String typeface(int style) {
        switch (style) {
            case DEFAULT: return "";
            case 1: return "serif-monospace";       // MonospacedWithSerifs
            case 2: return "serif";                 // ProportionalWithSerifs
            case 3: return "sans-serif-monospace";  // MonospacedWithoutSerifs
            case 4: return "sans-serif";            // ProportionalWithoutSerifs
            case 5: return "casual";                // Casual
            case 6: return "cursive";               // Cursive
            case 7: return "sans-serif-smallcaps";  // SmallCapitals
            default: return null;
        }
    }

    /** ClosedCaptionSize as a font scale; NaN for Default (no key), null when unknown. */
    static Float fontScale(int size) {
        switch (size) {
            case DEFAULT: return Float.NaN;
            case 1: return 0.5f;  // FiftyPercent
            case 2: return 1.0f;  // OneHundredPercent
            case 3: return 1.5f;  // OneHundredFiftyPercent
            case 4: return 2.0f;  // TwoHundredPercent
            default: return null;
        }
    }
}
