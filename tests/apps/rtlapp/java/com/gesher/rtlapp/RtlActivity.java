/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.rtlapp;

import android.app.Activity;
import android.content.res.Configuration;
import android.graphics.Paint;
import android.os.Bundle;
import android.os.LocaleList;
import android.text.Layout;
import android.text.SpannableStringBuilder;
import android.text.Spanned;
import android.text.style.ForegroundColorSpan;
import android.util.Log;
import android.view.View;
import android.view.ViewTreeObserver;
import android.widget.EditText;
import android.widget.TextView;

import java.text.NumberFormat;
import java.util.Calendar;
import java.util.Date;
import java.util.Locale;

/**
 * The right-to-left and scripts test app (docs/fonts.md): sets the texts of its layout
 * (colored runs of Hebrew, Arabic and Latin, Arabic joined and not, CJK, emoji), then,
 * once the window has laid it out, reports on stdout where each view is in the window,
 * the configuration's locales and layout direction, the formats they give, how its text
 * views laid their text out, and every move of the EditText's cursor
 * (tests/windows/tracks/39-rtl.ps1 reads the window back and posts the keys).
 */
public class RtlActivity extends Activity {
    private static final String TAG = "RtlActivity";

    static final int RED = 0xFFFF0000;
    static final int GREEN = 0xFF00A000;
    static final int MAGENTA = 0xFFFF00FF;
    static final int BLUE = 0xFF0000FF;

    // Hebrew shalom and olam; Arabic marhaba and bil-alam; the Arabic letter seen.
    static final String SHALOM = "שלום";
    static final String OLAM = "עולם";
    static final String MARHABA = "مرحبا";
    static final String BILALAM = "بالعالم";
    static final String SEEN = "س";
    // Chinese, Japanese, Korean; an unassigned code point no font has; three emoji.
    static final String CJK = "中文日本語한국어";
    static final String TOFU = "͸͸͸͸";
    static final String EMOJI = "😀🎉👍";

    static void say(String message) {
        System.out.println(message);
        System.out.flush();
        Log.i(TAG, message);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.main);
        text(R.id.hebrew, SHALOM, RED, " ", 0, OLAM, BLUE);
        text(R.id.arabic, MARHABA, RED, " ", 0, BILALAM, BLUE);
        text(R.id.mixed, SHALOM, RED, " ", 0, "ABC", GREEN, " ", 0, "123", MAGENTA, " ", 0,
                OLAM, BLUE);
        ((TextView) findViewById(R.id.joined)).setText(SEEN + SEEN + SEEN + SEEN);
        ((TextView) findViewById(R.id.isolated)).setText(
                SEEN + "‌" + SEEN + "‌" + SEEN + "‌" + SEEN);
        ((TextView) findViewById(R.id.gravity_end)).setText(SHALOM);
        ((TextView) findViewById(R.id.cjk)).setText(CJK);
        ((TextView) findViewById(R.id.tofu)).setText(TOFU);
        ((TextView) findViewById(R.id.emoji)).setText(EMOJI);
        final EditText edit = findViewById(R.id.edit);
        edit.setText(SHALOM);
        edit.setSelection(0);
        edit.requestFocus();

        final View root = findViewById(android.R.id.content);
        root.getViewTreeObserver().addOnGlobalLayoutListener(
                new ViewTreeObserver.OnGlobalLayoutListener() {
                    @Override
                    public void onGlobalLayout() {
                        root.getViewTreeObserver().removeOnGlobalLayoutListener(this);
                        report(root);
                    }
                });
        say("RtlActivity onCreate");
    }

    /** Sets a TextView's text: pairs of (text, color; 0 for the default color). */
    private void text(int id, Object... runs) {
        SpannableStringBuilder b = new SpannableStringBuilder();
        for (int i = 0; i < runs.length; i += 2) {
            int start = b.length();
            b.append((String) runs[i]);
            int color = (Integer) runs[i + 1];
            if (color != 0) {
                b.setSpan(new ForegroundColorSpan(color), start, b.length(),
                        Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
            }
        }
        ((TextView) findViewById(id)).setText(b);
    }

    private void report(View root) {
        Configuration config = getResources().getConfiguration();
        LocaleList locales = config.getLocales();
        say("RtlActivity locales " + locales.toLanguageTags() + ", default "
                + Locale.getDefault().toLanguageTag() + ", LocaleList default "
                + LocaleList.getDefault().toLanguageTags() + ", layout direction "
                + direction(config.getLayoutDirection()) + ", root "
                + direction(root.getLayoutDirection()) + ", resources "
                + getString(R.string.direction) + ", supportsRtl "
                + getApplicationInfo().hasRtlSupport());
        Calendar fixed = Calendar.getInstance();
        fixed.set(2026, Calendar.OCTOBER, 24, 15, 30, 0);
        Date when = fixed.getTime();
        say("RtlActivity formats: 24h " + android.text.format.DateFormat.is24HourFormat(this)
                + ", first day of week " + Calendar.getInstance().getFirstDayOfWeek()
                + " (icu " + android.icu.util.Calendar.getInstance().getFirstDayOfWeek()
                + "), number " + NumberFormat.getInstance().format(1234567.5)
                + ", time " + android.text.format.DateFormat.getTimeFormat(this).format(when)
                + ", date " + android.text.format.DateFormat.getDateFormat(this).format(when));
        String[] names = {"mirror", "mirror_first", "forced", "hebrew", "arabic", "mixed",
                "joined", "isolated", "latin_rtl", "view_start", "gravity_end", "cjk", "tofu",
                "emoji", "edit"};
        int[] ids = {R.id.mirror, R.id.mirror_first, R.id.forced, R.id.hebrew, R.id.arabic,
                R.id.mixed, R.id.joined, R.id.isolated, R.id.latin_rtl, R.id.view_start,
                R.id.gravity_end, R.id.cjk, R.id.tofu, R.id.emoji, R.id.edit};
        int[] at = new int[2];
        for (int i = 0; i < names.length; i++) {
            View v = findViewById(ids[i]);
            v.getLocationInWindow(at);
            StringBuilder line = new StringBuilder("RtlActivity view " + names[i] + " " + at[0]
                    + " " + at[1] + " " + v.getWidth() + " " + v.getHeight());
            if (v instanceof TextView && ((TextView) v).getLayout() != null) {
                TextView t = (TextView) v;
                Layout l = t.getLayout();
                line.append(", paragraph ").append(l.getParagraphDirection(0) < 0 ? "rtl" : "ltr")
                        .append(", line ").append(l.getLineLeft(0)).append("..")
                        .append(l.getLineRight(0)).append(", width ")
                        .append(t.getPaint().measureText(t.getText().toString()));
            }
            say(line.toString());
        }
        Paint paint = ((TextView) findViewById(R.id.cjk)).getPaint();
        say("RtlActivity glyphs: cjk " + paint.hasGlyph(CJK.substring(0, 1)) + "/"
                + paint.hasGlyph(CJK.substring(2, 3)) + "/" + paint.hasGlyph(CJK.substring(5, 6))
                + ", emoji " + paint.hasGlyph(EMOJI.substring(0, 2))
                + ", tofu " + paint.hasGlyph(TOFU.substring(0, 1))
                + ", arabic " + paint.hasGlyph(SEEN));
        EditText edit = findViewById(R.id.edit);
        // The cursor at offset 0, the RTL text's right end (taking focus moved it to the
        // end), for the arrow keys the test posts.
        edit.setSelection(0);
        Layout l = edit.getLayout();
        if (l != null) {
            say("RtlActivity edit: paragraph " + (l.getParagraphDirection(0) < 0 ? "rtl" : "ltr")
                    + ", cursor x at 0: " + l.getPrimaryHorizontal(0) + ", at "
                    + SHALOM.length() + ": " + l.getPrimaryHorizontal(SHALOM.length())
                    + ", focused " + edit.isFocused() + ", selection "
                    + edit.getSelectionStart() + "-" + edit.getSelectionEnd());
        }
    }

    private static String direction(int d) {
        return d == View.LAYOUT_DIRECTION_RTL ? "rtl" : d == View.LAYOUT_DIRECTION_LTR ? "ltr"
                : String.valueOf(d);
    }

    @Override
    protected void onResume() {
        super.onResume();
        say("RtlActivity onResume");
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        say("RtlActivity onDestroy");
    }
}
