/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.hardware.camera2.CameraMetadata;

import java.util.ArrayDeque;

/**
 * Android's autofocus state machine (CaptureResult.CONTROL_AF_STATE) over a Windows camera
 * whose driver really focuses automatically ({@link WindowsCamera.Device#autofocus}), for
 * Camera2 ({@link CameraDeviceUser}) and the legacy API ({@link LegacyCamera}).
 *
 * <p>The driver does the focusing: {@link Lens#auto} turns its automatic focus on and
 * {@link Lens#lock} turns it off with the lens kept where it reached. What a UVC focus
 * control does not report is whether the scan is over, so Gesher observes it: a scan has
 * converged when, for {@link #SETTLE_MS}, the lens position the driver reports does not
 * change and the picture's sharpness ({@link #sharpness}, the centre's normalised gradient)
 * stays within {@link #SETTLE_TOLERANCE}; it has failed when that does not happen within
 * {@link #MAX_SCAN_MS} (a featureless or dark picture never converges). A converged scan
 * is locked and then checked: the picture must keep its sharpness, else the result is
 * NOT_FOCUSED_LOCKED. Results therefore describe what the camera did, never a success that
 * was not observed.
 *
 * <p>Modes: AUTO (focus only on AF_TRIGGER_START, then locked), CONTINUOUS_VIDEO and
 * CONTINUOUS_PICTURE (the driver focuses continuously; a trigger locks it, immediately or,
 * for CONTINUOUS_PICTURE during a scan, once the scan ends). Any other mode keeps the lens
 * still (automatic focus off) with the state INACTIVE. A mode change resets the state to
 * INACTIVE, as on Android.
 *
 * <p>Not thread-safe: one capture thread drives it, frame by frame.
 */
public final class CameraAutoFocus {
    /** The device's focus control (WindowsCamera.Capture.focus). */
    public interface Lens {
        /** Turns the driver's automatic focus on; whether it did. */
        boolean auto();

        /** Turns automatic focus off with the lens kept where it is; whether it did. */
        boolean lock();

        /** The lens position the driver reports, or null when it reports none. */
        Integer position();
    }

    /** A scan lasts at least this long, so a driver slow to start moving is not cut short. */
    public static final long MIN_SCAN_MS = 600;
    /** How long the lens and the picture must stay still for a scan to have converged. */
    public static final long SETTLE_MS = 400;
    /** A scan that has not converged by then has failed. */
    public static final long MAX_SCAN_MS = 4000;
    /** How long after locking the picture is checked against the converged sharpness. */
    public static final long VERIFY_MS = 150;
    /** The relative sharpness spread allowed while converged. */
    public static final double SETTLE_TOLERANCE = 0.08;
    /** The relative sharpness change that starts a new passive scan. */
    public static final double CHANGE_TOLERANCE = 0.2;
    /** The share of the converged sharpness a locked picture must keep. */
    public static final double VERIFY_RATIO = 0.8;
    /** How often the driver is asked for the lens position while it matters. */
    public static final long POSITION_INTERVAL_MS = 100;

    private static final int OFF = CameraMetadata.CONTROL_AF_MODE_OFF;
    private static final int AUTO = CameraMetadata.CONTROL_AF_MODE_AUTO;
    private static final int PICTURE = CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_PICTURE;
    private static final int VIDEO = CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_VIDEO;

    private static final int INACTIVE = CameraMetadata.CONTROL_AF_STATE_INACTIVE;
    private static final int PASSIVE_SCAN = CameraMetadata.CONTROL_AF_STATE_PASSIVE_SCAN;
    private static final int PASSIVE_FOCUSED = CameraMetadata.CONTROL_AF_STATE_PASSIVE_FOCUSED;
    private static final int ACTIVE_SCAN = CameraMetadata.CONTROL_AF_STATE_ACTIVE_SCAN;
    private static final int FOCUSED_LOCKED = CameraMetadata.CONTROL_AF_STATE_FOCUSED_LOCKED;
    private static final int NOT_FOCUSED_LOCKED =
            CameraMetadata.CONTROL_AF_STATE_NOT_FOCUSED_LOCKED;
    private static final int PASSIVE_UNFOCUSED =
            CameraMetadata.CONTROL_AF_STATE_PASSIVE_UNFOCUSED;

    private static final int START = CameraMetadata.CONTROL_AF_TRIGGER_START;
    private static final int CANCEL = CameraMetadata.CONTROL_AF_TRIGGER_CANCEL;

    private static final class Sample {
        final long time;
        final double sharpness;
        final Integer position;

        Sample(long time, double sharpness, Integer position) {
            this.time = time;
            this.sharpness = sharpness;
            this.position = position;
        }
    }

    private final Lens mLens;
    private final ArrayDeque<Sample> mSamples = new ArrayDeque<>();
    private int mMode = -1;
    private int mState = INACTIVE;
    /** Whether the driver's automatic focus is on (as last set). */
    private boolean mAutoOn;
    private long mScanStart;
    /** CONTINUOUS_PICTURE: a trigger came during a passive scan; lock when it ends. */
    private boolean mPendingLock;
    /** Locked after converging; the picture is checked at mLockAt + VERIFY_MS. */
    private boolean mVerifying;
    private long mLockAt;
    private double mSettledSharpness;
    /** What a passive state is compared with to see the driver start a new scan. */
    private double mReferenceSharpness;
    private Integer mReferencePosition;
    private long mPositionAt = Long.MIN_VALUE / 2;

    public CameraAutoFocus(Lens lens) {
        mLens = lens;
    }

    /** The focus control of an open Windows camera; failures are logged once each. */
    static Lens lensOf(WindowsCamera.Capture capture, String camera) {
        return new Lens() {
            private final int[] mOut = new int[2];
            private boolean mAutoLogged, mLockLogged;

            @Override
            public boolean auto() {
                int hr = capture.focus(WindowsCamera.Capture.FOCUS_AUTO, mOut);
                if (hr < 0 && !mAutoLogged) {
                    mAutoLogged = true;
                    CameraService.log(String.format("camera %s: automatic focus on failed:"
                            + " HRESULT 0x%08X", camera, hr));
                }
                return hr >= 0;
            }

            @Override
            public boolean lock() {
                int hr = capture.focus(WindowsCamera.Capture.FOCUS_LOCK, mOut);
                if (hr < 0 && !mLockLogged) {
                    mLockLogged = true;
                    CameraService.log(String.format("camera %s: locking focus failed:"
                            + " HRESULT 0x%08X", camera, hr));
                }
                return hr >= 0;
            }

            @Override
            public Integer position() {
                return capture.focus(WindowsCamera.Capture.FOCUS_GET, mOut) >= 0 ? mOut[0] : null;
            }
        };
    }

    /** Whether {@code mode} is one this machine runs the driver's focus for. */
    public static boolean isAutofocusMode(int mode) {
        return mode == AUTO || mode == PICTURE || mode == VIDEO;
    }

    /**
     * One captured frame: applies the request's AF mode and trigger, takes in this frame's
     * sharpness and the lens position, and returns the AF state for the frame's result.
     */
    public int frame(int afMode, int trigger, long nowMs, double sharpness) {
        int mode = isAutofocusMode(afMode) ? afMode : OFF;
        boolean entered = mode != mMode;
        if (entered) enter(mode, nowMs);
        if (mode == OFF) return mState;
        // The driver is asked (a USB control request on UVC cameras) only while the
        // lens position can matter, not while locked or idle in AUTO, and at most every
        // POSITION_INTERVAL_MS; a frame without a reading is judged by its picture.
        boolean idle = mState == FOCUSED_LOCKED || mState == NOT_FOCUSED_LOCKED
                || (mode == AUTO && mState == INACTIVE);
        Integer position = null;
        if (!idle && (entered || nowMs - mPositionAt >= POSITION_INTERVAL_MS)) {
            position = mLens.position();
            mPositionAt = nowMs;
        }
        record(new Sample(nowMs, sharpness, position));
        if (mode == AUTO) {
            auto(trigger, nowMs, sharpness);
        } else {
            continuous(mode, trigger, nowMs, sharpness, position, entered);
        }
        return mState;
    }

    /** The current state (CONTROL_AF_STATE). */
    public int state() {
        return mState;
    }

    /**
     * Whether the lens is moving (LENS_STATE): during a scan, until it is locked, and in a
     * locked state whose lock the driver refused (its automatic focus still runs).
     */
    public boolean lensMoving() {
        if ((mState == FOCUSED_LOCKED || mState == NOT_FOCUSED_LOCKED) && mAutoOn) return true;
        return (mState == ACTIVE_SCAN || mState == PASSIVE_SCAN) && !mVerifying;
    }

    private void enter(int mode, long now) {
        mMode = mode;
        mState = INACTIVE;
        mPendingLock = false;
        mVerifying = false;
        mSamples.clear();
        mScanStart = now;
        if (mode == PICTURE || mode == VIDEO) {
            mAutoOn = mLens.auto();
        } else {
            // OFF and AUTO without a trigger: the lens stays where it is.
            mAutoOn = !mLens.lock();
        }
    }

    private void record(Sample sample) {
        mSamples.add(sample);
        // Keep one sample at or before the window's start, to know the window is covered.
        while (mSamples.size() > 1) {
            Sample oldest = mSamples.poll();
            Sample next = mSamples.peek();
            if (next.time > sample.time - SETTLE_MS) {
                mSamples.addFirst(oldest);
                break;
            }
        }
    }

    private void restartScan(long now) {
        mScanStart = now;
        mSamples.clear();
    }

    /** Locks the lens; when the driver refuses, its automatic focus is still on. */
    private boolean lockLens() {
        boolean locked = mLens.lock();
        if (locked) mAutoOn = false;
        return locked;
    }

    /** The converged sharpness, or null while the lens or the picture still changes. */
    private Double settled(long now) {
        if (now - mScanStart < MIN_SCAN_MS || mSamples.size() < 3) return null;
        Sample first = mSamples.peek();
        if (first.time > now - SETTLE_MS || first.time < mScanStart) return null;
        double min = Double.MAX_VALUE, max = 0, sum = 0;
        Integer position = null;
        for (Sample s : mSamples) {
            if (s.position != null) {
                if (position != null && !position.equals(s.position)) return null;
                position = s.position;
            }
            min = Math.min(min, s.sharpness);
            max = Math.max(max, s.sharpness);
            sum += s.sharpness;
        }
        if (max <= 0 || max - min > SETTLE_TOLERANCE * max) return null;
        return sum / mSamples.size();
    }

    /** A scan's end, locking: FOCUSED_LOCKED or NOT_FOCUSED_LOCKED once known. */
    private void progressToLock(long now, double sharpness) {
        if (mVerifying) {
            if (now - mLockAt >= VERIFY_MS) {
                mVerifying = false;
                mPendingLock = false;
                mState = sharpness >= VERIFY_RATIO * mSettledSharpness ? FOCUSED_LOCKED
                        : NOT_FOCUSED_LOCKED;
            }
            return;
        }
        Double converged = settled(now);
        if (converged != null) {
            if (!lockLens()) {
                mPendingLock = false;
                mState = NOT_FOCUSED_LOCKED;
                return;
            }
            mVerifying = true;
            mLockAt = now;
            mSettledSharpness = converged;
        } else if (now - mScanStart >= MAX_SCAN_MS) {
            lockLens();
            mPendingLock = false;
            mState = NOT_FOCUSED_LOCKED;
        }
    }

    private void auto(int trigger, long now, double sharpness) {
        if (trigger == START) {
            if (mState == ACTIVE_SCAN) return;  // already scanning for a trigger
            mVerifying = false;
            // This frame was exposed before the scan: it does not count.
            restartScan(now);
            mAutoOn = mLens.auto();
            if (!mAutoOn) {
                lockLens();
                mState = NOT_FOCUSED_LOCKED;
                return;
            }
            mState = ACTIVE_SCAN;
        } else if (trigger == CANCEL) {
            if (mState == ACTIVE_SCAN) lockLens();
            mVerifying = false;
            mState = INACTIVE;
        } else if (mState == ACTIVE_SCAN) {
            progressToLock(now, sharpness);
        }
    }

    private void continuous(int mode, int trigger, long now, double sharpness,
            Integer position, boolean entered) {
        boolean locked = mState == FOCUSED_LOCKED || mState == NOT_FOCUSED_LOCKED;
        if (trigger == CANCEL) {
            mPendingLock = false;
            mVerifying = false;
            if (!mAutoOn) mAutoOn = mLens.auto();
            mState = INACTIVE;
            restartScan(now);
            return;
        }
        if (trigger == START) {
            if (locked || mPendingLock) return;
            if (mState == PASSIVE_FOCUSED) {
                mState = lockLens() ? FOCUSED_LOCKED : NOT_FOCUSED_LOCKED;
            } else if (mState == PASSIVE_SCAN && mode == PICTURE) {
                mPendingLock = true;  // locks when this scan ends
            } else {
                lockLens();
                mState = NOT_FOCUSED_LOCKED;
            }
            return;
        }
        if (mPendingLock) {
            // CONTINUOUS_PICTURE's trigger during a scan: its end locks (mAutoOn goes
            // false at the lock, while the result is still being checked).
            progressToLock(now, sharpness);
            return;
        }
        // The frame that changed the mode reports INACTIVE, as Android's mode change does.
        if (locked || !mAutoOn || entered) return;
        if (mState == INACTIVE) {
            // The driver's continuous focus is on: it scans from here.
            mState = PASSIVE_SCAN;
            restartScan(now);
        } else if (mState == PASSIVE_SCAN) {
            Double converged = settled(now);
            if (converged != null) {
                mState = PASSIVE_FOCUSED;
                mReferenceSharpness = converged;
                mReferencePosition = position;
            } else if (now - mScanStart >= MAX_SCAN_MS) {
                mState = PASSIVE_UNFOCUSED;
                mReferenceSharpness = sharpness;
                mReferencePosition = position;
            }
        } else if (changed(sharpness, position)) {
            // PASSIVE_FOCUSED or PASSIVE_UNFOCUSED: the lens or the scene moved.
            mState = PASSIVE_SCAN;
            restartScan(now);
        }
    }

    private boolean changed(double sharpness, Integer position) {
        if (position != null && mReferencePosition != null
                && !position.equals(mReferencePosition)) {
            return true;
        }
        double reference = Math.max(mReferenceSharpness, 1e-9);
        return Math.abs(sharpness - mReferenceSharpness) > CHANGE_TOLERANCE * reference;
    }

    /**
     * The sharpness of an NV12 frame's centre (its middle half in both directions): the
     * mean absolute difference between horizontally and vertically adjacent luma samples,
     * divided by the mean luma, so exposure changes do not read as focus changes. 0 for a
     * black or featureless picture.
     */
    public static double sharpness(byte[] nv12, int width, int height) {
        if (nv12 == null || width < 8 || height < 8 || nv12.length < width * height) return 0;
        int x0 = width / 4, x1 = width * 3 / 4, y0 = height / 4, y1 = height * 3 / 4;
        long area = (long) (x1 - x0) * (y1 - y0);
        // About 40000 samples whatever the frame size.
        int step = Math.max(1, (int) Math.sqrt(area / 40000.0));
        long gradient = 0, luma = 0;
        for (int y = y0; y < y1; y += step) {
            int row = y * width;
            for (int x = x0; x < x1; x += step) {
                int p = nv12[row + x] & 0xff;
                gradient += Math.abs(p - (nv12[row + x + 1] & 0xff))
                        + Math.abs(p - (nv12[row + width + x] & 0xff));
                luma += p;
            }
        }
        return luma == 0 ? 0 : (double) gradient / luma;
    }
}
