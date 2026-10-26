/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.devsvc;

import android.app.Activity;
import android.content.Context;
import android.os.Bundle;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;

/** The vibrator, as on a device without one (hwsvc2, ADR 0051). */
public class VibrateActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        try {
            Vibrator v = (Vibrator) getSystemService(Context.VIBRATOR_SERVICE);
            VibratorManager vm = getSystemService(VibratorManager.class);
            Report.line("vibrator: " + (v != null ? v.getClass().getSimpleName() : "null")
                    + " hasVibrator=" + (v != null && v.hasVibrator())
                    + " hasAmplitudeControl=" + (v != null && v.hasAmplitudeControl()));
            Report.line("vibrator: manager " + (vm != null ? vm.getClass().getSimpleName() : "null")
                    + " ids=" + (vm != null ? vm.getVibratorIds().length : -1)
                    + " default hasVibrator=" + (vm != null && vm.getDefaultVibrator().hasVibrator())
                    + " vibrator(0)=" + (vm != null ? vm.getVibrator(0).getClass().getSimpleName()
                            : "null"));
            if (v != null) {
                v.vibrate(VibrationEffect.createOneShot(200, VibrationEffect.DEFAULT_AMPLITUDE));
                v.vibrate(VibrationEffect.createWaveform(new long[] {0, 100, 50, 100}, -1));
                v.cancel();
            }
            Report.line("vibrator: vibrate and cancel returned");
        } catch (Throwable t) {
            Report.line("vibrator: failed: " + t);
        }
        finish();
    }
}
