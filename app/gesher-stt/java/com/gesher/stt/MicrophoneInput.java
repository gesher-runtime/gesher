/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.stt;

import android.content.Context;
import android.content.ContextParams;
import android.content.AttributionSource;
import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.MediaRecorder;

import java.io.IOException;

/**
 * The microphone, through Android's AudioRecord (Gesher's microphone path), opened with
 * an attribution context for the calling app, as RecognitionService.onStartListening's
 * documentation asks: the app is blamed for the microphone and this service is its proxy.
 */
final class MicrophoneInput implements AudioInput {
    static final int SAMPLE_RATE = 16000;

    private final Context mContext;
    private final AttributionSource mCaller;
    private volatile AudioRecord mRecord;
    private volatile boolean mStopped;

    MicrophoneInput(Context context, AttributionSource caller) {
        mContext = context;
        mCaller = caller;
    }

    @Override
    public int sampleRate() {
        return SAMPLE_RATE;
    }

    @Override
    public void start() throws IOException {
        int min = AudioRecord.getMinBufferSize(SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT);
        if (min <= 0) throw new IOException("AudioRecord.getMinBufferSize: " + min);
        Context attribution = mContext.createContext(new ContextParams.Builder()
                .setNextAttributionSource(mCaller).build());
        AudioRecord record;
        try {
            record = new AudioRecord.Builder()
                    .setContext(attribution)
                    .setAudioSource(MediaRecorder.AudioSource.VOICE_RECOGNITION)
                    .setAudioFormat(new AudioFormat.Builder()
                            .setSampleRate(SAMPLE_RATE)
                            .setChannelMask(AudioFormat.CHANNEL_IN_MONO)
                            .setEncoding(AudioFormat.ENCODING_PCM_16BIT).build())
                    .setBufferSizeInBytes(Math.max(min, SAMPLE_RATE / 10 * 2 * 4))
                    .build();
        } catch (UnsupportedOperationException | IllegalArgumentException | SecurityException e) {
            throw new IOException("AudioRecord: " + e, e);
        }
        if (record.getState() != AudioRecord.STATE_INITIALIZED) {
            record.release();
            throw new IOException("AudioRecord is not initialized");
        }
        mRecord = record;
        try {
            record.startRecording();
        } catch (IllegalStateException e) {
            throw new IOException("AudioRecord.startRecording: " + e, e);
        }
        if (record.getRecordingState() != AudioRecord.RECORDSTATE_RECORDING) {
            throw new IOException("AudioRecord did not start recording");
        }
        if (mStopped) record.stop();
    }

    @Override
    public int read(short[] pcm) throws IOException {
        AudioRecord record = mRecord;
        if (record == null || mStopped) return 0;
        int n = record.read(pcm, 0, pcm.length);
        if (n < 0) {
            if (mStopped) return 0;
            throw new IOException("AudioRecord.read: " + n);
        }
        return n;
    }

    @Override
    public void stop() {
        mStopped = true;
        AudioRecord record = mRecord;
        if (record == null) return;
        try {
            record.stop();
        } catch (IllegalStateException alreadyStopped) {
            // Released or never started.
        }
    }

    /** The recorder's release, from the session thread once it reads no more. */
    void release() {
        AudioRecord record = mRecord;
        mRecord = null;
        if (record != null) record.release();
    }
}
