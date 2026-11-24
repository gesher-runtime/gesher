/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.stt;

import android.content.AttributionSource;
import android.content.Intent;
import android.media.AudioFormat;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.os.RemoteException;
import android.os.SystemClock;
import android.speech.ModelDownloadListener;
import android.speech.RecognitionService;
import android.speech.RecognitionSupport;
import android.speech.RecognizerIntent;
import android.speech.SpeechRecognizer;
import android.util.Log;

import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;

/**
 * Gesher's RecognitionService (docs/speech-recognition.md): dictation by Windows' SAPI
 * recognizers, over audio that AudioRecord captures for the calling app.
 *
 * <p>RecognitionService (android-16.0.0_r4) calls onStartListening before it checks the
 * caller's RECORD_AUDIO (checkPermissionAndStartDataDelivery), and calls onCancel when that
 * check fails. Nothing is opened in onStartListening itself: the session starts from the
 * main handler afterwards, so a refused caller gets ERROR_INSUFFICIENT_PERMISSIONS and
 * nothing else, and no microphone is opened for it. For the same reason the attribution
 * context is created only when the microphone is opened: created in onStartListening, it
 * would let RecognitionService skip the caller's permission check.
 *
 * <p>One session at a time (RecognitionService's default getMaxConcurrentSessionsCount):
 * a second caller gets ERROR_RECOGNIZER_BUSY from RecognitionService.
 */
public final class WindowsRecognitionService extends RecognitionService {
    private static final String TAG = "GesherSpeech";

    /** No sound within this long after onReadyForSpeech: ERROR_SPEECH_TIMEOUT. */
    static final long NO_SPEECH_TIMEOUT_MS = 5_000;
    /** Sound ended but Windows has not answered: the audio ends, Windows answers what it has. */
    static final long AFTER_SOUND_END_MS = 2_000;
    /** The longest utterance: the audio ends after this long of speech. */
    static final long MAX_SPEECH_MS = 60_000;
    /** After the audio ended, how long Windows may take to finish. */
    static final long FINISH_TIMEOUT_MS = 20_000;
    /** PCM per read: 100 ms. */
    static final int CHUNK_MS = 100;
    /** Results requested by default (EXTRA_MAX_RESULTS absent). */
    static final int DEFAULT_MAX_RESULTS = 5;

    private final RecognizerBackend mBackend;
    private final Handler mMain = new Handler(Looper.getMainLooper());
    private Session mSession;  // main thread

    public WindowsRecognitionService() {
        this(new WindowsRecognizerBackend());
    }

    WindowsRecognitionService(RecognizerBackend backend) {
        mBackend = backend;
    }

    /** The audio of a session; the microphone unless the app passed its own. */
    AudioInput openInput(Intent intent, Callback callback) throws InputRefused {
        ParcelFileDescriptor source = intent.getParcelableExtra(
                RecognizerIntent.EXTRA_AUDIO_SOURCE, ParcelFileDescriptor.class);
        if (source == null) {
            return new MicrophoneInput(this, callback.getCallingAttributionSource());
        }
        int encoding = intent.getIntExtra(RecognizerIntent.EXTRA_AUDIO_SOURCE_ENCODING,
                AudioFormat.ENCODING_PCM_16BIT);
        int channels = intent.getIntExtra(RecognizerIntent.EXTRA_AUDIO_SOURCE_CHANNEL_COUNT, 1);
        int rate = intent.getIntExtra(RecognizerIntent.EXTRA_AUDIO_SOURCE_SAMPLING_RATE, 16000);
        if (!StreamInput.accepts(encoding, channels, rate)) {
            try {
                source.close();
            } catch (IOException ignored) {
                // Closing the app's descriptor is all that is left to do.
            }
            throw new InputRefused("audio source " + encoding + "/" + channels + "ch/" + rate + "Hz");
        }
        return new StreamInput(source, rate);
    }

    static final class InputRefused extends Exception {
        InputRefused(String message) {
            super(message);
        }
    }

    @Override
    protected void onStartListening(Intent recognizerIntent, Callback listener) {
        Session session = new Session(recognizerIntent, listener);
        mSession = session;
        // After RecognitionService's permission check (see the class comment).
        mMain.post(session::begin);
    }

    @Override
    protected void onStopListening(Callback listener) {
        Session session = mSession;
        if (session != null && session.callback == listener) session.stop();
    }

    @Override
    protected void onCancel(Callback listener) {
        Session session = mSession;
        if (session != null && session.callback == listener) {
            mSession = null;
            session.cancel();
        }
    }

    @Override
    public void onDestroy() {
        Session session = mSession;
        mSession = null;
        if (session != null) session.cancel();
        super.onDestroy();
    }

    @Override
    public void onCheckRecognitionSupport(Intent recognizerIntent,
            AttributionSource attributionSource, SupportCallback supportCallback) {
        // Every recognizer is SAPI's, installed on this PC: installed on-device languages.
        // Nothing is pending, downloadable or online.
        supportCallback.onSupportResult(new RecognitionSupport.Builder()
                .setInstalledOnDeviceLanguages(Languages.tags(mBackend.languages()))
                .build());
    }

    @Override
    public void onTriggerModelDownload(Intent recognizerIntent,
            AttributionSource attributionSource, ModelDownloadListener listener) {
        // A recognizer of the language is installed: the model is available. Otherwise
        // Windows has none, and this service cannot download one (Windows installs
        // recognizers with its speech language features).
        if (Languages.match(mBackend.languages(), requestedLanguage(recognizerIntent)) != null) {
            listener.onSuccess();
        } else {
            listener.onError(SpeechRecognizer.ERROR_LANGUAGE_NOT_SUPPORTED);
        }
    }

    static String requestedLanguage(Intent intent) {
        String tag = intent.getStringExtra(RecognizerIntent.EXTRA_LANGUAGE);
        return tag != null && !tag.isEmpty() ? tag : Locale.getDefault().toLanguageTag();
    }

    /** dBFS of 16-bit PCM: the real level the microphone delivered. */
    static float levelDb(short[] pcm, int count) {
        if (count <= 0) return -100f;
        double sum = 0;
        for (int i = 0; i < count; i++) sum += (double) pcm[i] * pcm[i];
        double rms = Math.sqrt(sum / count);
        if (rms < 1) return -100f;
        return (float) Math.max(-100.0, 20 * Math.log10(rms / 32767.0));
    }

    /** One startListening, from the main handler's begin to its single final callback. */
    final class Session implements Runnable {
        final Intent intent;
        final Callback callback;
        volatile boolean stopRequested;
        private volatile boolean mCanceled;
        private volatile AudioInput mInput;
        private volatile RecognizerBackend.Recognition mRecognition;
        // Main thread:
        private boolean mFinished, mBegun, mEnded;

        Session(Intent intent, Callback callback) {
            this.intent = intent;
            this.callback = callback;
        }

        void begin() {
            if (mCanceled) return;
            Thread worker = new Thread(this, "gesher-speech-session");
            worker.setDaemon(true);
            worker.start();
        }

        /** stopListening: the capture ends now; Windows recognizes what it was handed. */
        void stop() {
            stopRequested = true;
            AudioInput input = mInput;
            if (input != null) input.stop();
        }

        void cancel() {
            mCanceled = true;
            AudioInput input = mInput;
            if (input != null) input.stop();
            RecognizerBackend.Recognition r = mRecognition;
            if (r != null) r.cancel();
        }

        // --- the session thread ------------------------------------------------------

        @Override
        public void run() {
            AudioInput input = null;
            RecognizerBackend.Recognition recognition = null;
            try {
                String tag = Languages.match(mBackend.languages(), requestedLanguage(intent));
                if (tag == null) {
                    post(() -> fail(SpeechRecognizer.ERROR_LANGUAGE_NOT_SUPPORTED));
                    return;
                }
                try {
                    input = openInput(intent, callback);
                } catch (InputRefused e) {
                    Log.w(TAG, "speech session: " + e.getMessage());
                    post(() -> fail(SpeechRecognizer.ERROR_AUDIO));
                    return;
                }
                mInput = input;
                if (mCanceled) return;
                boolean partials = intent.getBooleanExtra(RecognizerIntent.EXTRA_PARTIAL_RESULTS, false);
                int maxResults = Math.max(1, intent.getIntExtra(RecognizerIntent.EXTRA_MAX_RESULTS,
                        DEFAULT_MAX_RESULTS));
                try {
                    recognition = mBackend.start(tag, input.sampleRate(), partials, maxResults);
                } catch (RecognizerBackend.StartFailure e) {
                    Log.w(TAG, "speech session: " + e.getMessage());
                    int error = e.reason == RecognizerBackend.NO_RECOGNIZER
                            ? SpeechRecognizer.ERROR_LANGUAGE_NOT_SUPPORTED
                            : e.reason == RecognizerBackend.RECOGNIZER_FAILED
                            ? SpeechRecognizer.ERROR_LANGUAGE_UNAVAILABLE
                            : SpeechRecognizer.ERROR_SERVER;
                    post(() -> fail(error));
                    return;
                }
                mRecognition = recognition;
                if (mCanceled) return;
                try {
                    input.start();
                } catch (IOException e) {
                    Log.w(TAG, "speech session: " + e.getMessage());
                    post(() -> fail(SpeechRecognizer.ERROR_AUDIO));
                    return;
                }
                if (mCanceled) return;
                post(this::ready);
                listen(input, recognition, partials);
            } finally {
                if (input != null) {
                    input.stop();
                    if (input instanceof MicrophoneInput) ((MicrophoneInput) input).release();
                }
                if (recognition != null) {
                    recognition.cancel();
                    recognition.release();
                }
            }
        }

        private void listen(AudioInput input, RecognizerBackend.Recognition recognition,
                boolean partials) {
            short[] pcm = new short[input.sampleRate() * CHUNK_MS / 1000];
            long start = SystemClock.elapsedRealtime();
            long soundStart = -1, soundEnd = -1;
            boolean capturing = true;
            long finishBy = Long.MAX_VALUE;
            while (!mCanceled) {
                if (capturing) {
                    int n;
                    try {
                        n = input.read(pcm);
                    } catch (IOException e) {
                        Log.w(TAG, "speech session: " + e.getMessage());
                        post(() -> fail(SpeechRecognizer.ERROR_AUDIO));
                        return;
                    }
                    if (mCanceled) return;
                    if (n > 0) {
                        recognition.write(pcm, n);
                        float level = levelDb(pcm, n);
                        post(() -> rms(level));
                    }
                    long now = SystemClock.elapsedRealtime();
                    boolean end = n == 0 || stopRequested
                            || (soundStart < 0 && now - start >= NO_SPEECH_TIMEOUT_MS)
                            || (soundStart >= 0 && now - soundStart >= MAX_SPEECH_MS)
                            || (soundEnd >= 0 && now - soundEnd >= AFTER_SOUND_END_MS);
                    if (end) {
                        capturing = false;
                        input.stop();
                        recognition.endAudio();
                        finishBy = now + FINISH_TIMEOUT_MS;
                    }
                }
                RecognizerBackend.Event e = recognition.next(capturing ? 0 : 200);
                while (e.kind != RecognizerBackend.NONE && !mCanceled) {
                    switch (e.kind) {
                        case RecognizerBackend.SOUND_START:
                            if (soundStart < 0) soundStart = SystemClock.elapsedRealtime();
                            post(this::beginning);
                            break;
                        case RecognizerBackend.SOUND_END:
                            if (soundEnd < 0) soundEnd = SystemClock.elapsedRealtime();
                            post(this::end);
                            break;
                        case RecognizerBackend.HYPOTHESIS:
                            if (partials && e.texts.length > 0) {
                                String text = e.texts[0];
                                post(() -> partial(text));
                            }
                            break;
                        case RecognizerBackend.RECOGNITION: {
                            String[] texts = e.texts;
                            float[] confidences = e.confidences;
                            post(() -> results(texts, confidences));
                            return;
                        }
                        case RecognizerBackend.NO_MATCH:
                            post(() -> fail(SpeechRecognizer.ERROR_NO_MATCH));
                            return;
                        case RecognizerBackend.STREAM_END: {
                            boolean heard = soundStart >= 0;
                            post(() -> fail(heard ? SpeechRecognizer.ERROR_NO_MATCH
                                    : SpeechRecognizer.ERROR_SPEECH_TIMEOUT));
                            return;
                        }
                        default:  // FAILED
                            Log.w(TAG, "speech session: Windows recognition failed: "
                                    + String.format("HRESULT 0x%08X", e.hr));
                            post(() -> fail(SpeechRecognizer.ERROR_SERVER));
                            return;
                    }
                    e = recognition.next(0);
                }
                if (!capturing && SystemClock.elapsedRealtime() >= finishBy) {
                    Log.w(TAG, "speech session: Windows did not finish the stream");
                    post(() -> fail(SpeechRecognizer.ERROR_SERVER));
                    return;
                }
            }
        }

        private void post(Runnable r) {
            mMain.post(r);
        }

        // --- the main thread: the callbacks, in order, until the final one -----------

        private boolean live() {
            return !mCanceled && !mFinished && mSession == this;
        }

        private void ready() {
            if (!live()) return;
            deliver(() -> callback.readyForSpeech(new Bundle()));
        }

        private void beginning() {
            if (!live() || mBegun) return;
            mBegun = true;
            deliver(callback::beginningOfSpeech);
        }

        private void rms(float level) {
            if (!live()) return;
            deliver(() -> callback.rmsChanged(level));
        }

        private void partial(String text) {
            if (!live() || !mBegun || mEnded) return;
            Bundle b = new Bundle();
            b.putStringArrayList(SpeechRecognizer.RESULTS_RECOGNITION,
                    new ArrayList<>(List.of(text)));
            deliver(() -> callback.partialResults(b));
        }

        private void end() {
            if (!live() || !mBegun || mEnded) return;
            mEnded = true;
            deliver(callback::endOfSpeech);
        }

        private void results(String[] texts, float[] confidences) {
            if (!live()) return;
            end();
            if (!live()) return;
            mFinished = true;
            mSession = null;
            Bundle b = new Bundle();
            b.putStringArrayList(SpeechRecognizer.RESULTS_RECOGNITION,
                    new ArrayList<>(Arrays.asList(texts)));
            b.putFloatArray(SpeechRecognizer.CONFIDENCE_SCORES, confidences);
            deliver(() -> callback.results(b));
        }

        private void fail(int error) {
            if (!live()) return;
            end();
            if (!live()) return;
            mFinished = true;
            mSession = null;
            deliver(() -> callback.error(error));
        }

        private void deliver(RemoteCall call) {
            try {
                call.run();
            } catch (RemoteException | RuntimeException clientGone) {
                // The client is gone: nothing more is delivered, and Windows stops.
                if (mSession == this) mSession = null;
                mFinished = true;
                cancel();
            }
        }
    }

    @FunctionalInterface
    private interface RemoteCall {
        void run() throws RemoteException;
    }
}
