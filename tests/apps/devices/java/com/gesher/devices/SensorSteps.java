/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.devices;

import android.content.Context;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener2;
import android.hardware.SensorManager;
import android.os.Handler;
import android.os.HandlerThread;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

/**
 * SensorManager: the list (Windows' sensors, the test ones with GESHER_SENSOR_TEST, or
 * none), and for an accelerometer and a light sensor, events through a listener on a
 * Handler, flush and unregistering.
 */
final class SensorSteps {
    private final SensorManager mSm;
    private final HandlerThread mThread = new HandlerThread("sensor-callbacks");
    private final Handler mHandler;
    private final BlockingQueue<String> mEvents = new LinkedBlockingQueue<>();

    SensorSteps(Context context) {
        mSm = context.getSystemService(SensorManager.class);
        mThread.start();
        mHandler = new Handler(mThread.getLooper());
    }

    private final SensorEventListener2 mListener = new SensorEventListener2() {
        @Override
        public void onSensorChanged(SensorEvent event) {
            mEvents.add(String.format(Locale.ROOT, "event %s %.2f,%.2f,%.2f accuracy %d",
                    event.sensor.getName(), event.values[0],
                    event.values.length > 1 ? event.values[1] : 0f,
                    event.values.length > 2 ? event.values[2] : 0f, event.accuracy));
        }

        @Override
        public void onAccuracyChanged(Sensor sensor, int accuracy) {
        }

        @Override
        public void onFlushCompleted(Sensor sensor) {
            mEvents.add("flush " + sensor.getName());
        }
    };

    private String next(long timeoutMs) throws InterruptedException {
        String e = mEvents.poll(timeoutMs, TimeUnit.MILLISECONDS);
        return e != null ? e : "(nothing within " + timeoutMs + " ms)";
    }

    void run() throws Exception {
        List<Sensor> all = mSm.getSensorList(Sensor.TYPE_ALL);
        List<String> names = new ArrayList<>();
        for (Sensor s : all) names.add(s.getName() + " (" + s.getStringType() + ")");
        Report.line("sensors: " + all.size() + " sensors" + (all.isEmpty() ? "" : ": "
                + String.join(", ", names)));
        Sensor accel = mSm.getDefaultSensor(Sensor.TYPE_ACCELEROMETER);
        Sensor light = mSm.getDefaultSensor(Sensor.TYPE_LIGHT);
        if (accel == null) {
            Report.line("sensors: no accelerometer; registerListener answers "
                    + mSm.registerListener(mListener, accel, SensorManager.SENSOR_DELAY_GAME));
        } else {
            Report.line("sensors: accelerometer " + accel.getName() + ", range "
                    + accel.getMaximumRange() + ", min delay " + accel.getMinDelay() + " us");
            boolean ok = mSm.registerListener(mListener, accel, SensorManager.SENSOR_DELAY_GAME,
                    mHandler);
            Report.line("sensors: registerListener(accelerometer) " + ok + ", first "
                    + next(10000));
            int more = 0;
            for (int i = 0; i < 4; i++) {
                if (next(5000).startsWith("event ")) more++;
            }
            Report.line("sensors: accelerometer events keep coming: " + more + " more");
            mSm.flush(mListener);
            String f = next(5000);
            while (f.startsWith("event ")) f = next(5000);
            Report.line("sensors: flush: " + f);
            mSm.unregisterListener(mListener);
            mEvents.clear();
            Thread.sleep(300);
            mEvents.clear();
            Report.line("sensors: after unregisterListener " + next(500));
        }
        if (light != null) {
            mSm.registerListener(mListener, light, SensorManager.SENSOR_DELAY_NORMAL, mHandler);
            Report.line("sensors: light " + next(10000));
            mSm.unregisterListener(mListener, light);
        } else {
            Report.line("sensors: no light sensor");
        }
        Report.line("sensors: done");
        mThread.quitSafely();
    }
}
