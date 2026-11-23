/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.tts;

import android.speech.tts.SynthesisCallback;
import android.speech.tts.SynthesisRequest;
import android.speech.tts.TextToSpeech;
import android.speech.tts.TextToSpeechService;
import android.speech.tts.Voice;
import android.util.Log;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import java.util.Set;

/**
 * Gesher's text-to-speech engine (docs/tts.md): Android's TextToSpeechService contract
 * (android-16.0.0_r4, frameworks/base core/java/android/speech/tts) over Windows' speech
 * synthesis. The voices are exactly the PC's Windows voices; each utterance is rendered
 * by Windows into PCM, which is handed to the SynthesisCallback unchanged:
 * {@code start(sampleRate, encoding, channels)}, {@code rangeStart} for each word Windows
 * reports, {@code audioAvailable} in chunks of at most {@code getMaxBufferSize()}, then
 * {@code done()}; or {@code error(code)} then {@code done()}. TextToSpeechService turns
 * these into the client's onStart/onRangeStart/onDone/onError/onStop, played or written
 * to a file (synthesizeToFile) the same way. Synthesis is local: no network feature.
 */
public final class WindowsTtsService extends TextToSpeechService {
    private static final String TAG = "GesherTts";
    /** The longest a Windows rendering may take before the request fails. */
    static final long SYNTHESIS_TIMEOUT_MS = 120_000;

    private final SpeechBackend mBackend;
    private final Object mLock = new Object();
    // Guarded by mLock.
    private SpeechBackend.Synthesis mCurrent;
    private volatile VoiceCatalog mCatalog;
    private volatile String[] mLanguage = {"", "", ""};

    public WindowsTtsService() {
        this(new WindowsSpeechBackend());
    }

    WindowsTtsService(SpeechBackend backend) {
        mBackend = backend;
    }

    /** The voices; read again from Windows on {@code refresh} (onGetVoices). */
    private VoiceCatalog catalog(boolean refresh) {
        VoiceCatalog catalog = mCatalog;
        if (catalog == null || refresh) {
            catalog = VoiceCatalog.of(mBackend.voices());
            mCatalog = catalog;
        }
        return catalog;
    }

    @Override
    protected int onIsLanguageAvailable(String lang, String country, String variant) {
        return catalog(false).availability(lang, country, variant);
    }

    @Override
    protected String[] onGetLanguage() {
        return mLanguage.clone();
    }

    @Override
    protected int onLoadLanguage(String lang, String country, String variant) {
        int availability = catalog(false).availability(lang, country, variant);
        if (availability >= TextToSpeech.LANG_AVAILABLE) {
            mLanguage = new String[] {VoiceCatalog.iso3Language(lang),
                    availability == TextToSpeech.LANG_COUNTRY_AVAILABLE
                            ? VoiceCatalog.iso3Country(country) : "", ""};
        }
        return availability;
    }

    @Override
    protected Set<String> onGetFeaturesForLanguage(String lang, String country, String variant) {
        if (catalog(false).availability(lang, country, variant) < TextToSpeech.LANG_AVAILABLE) {
            return Collections.emptySet();
        }
        return Collections.singleton(TextToSpeech.Engine.KEY_FEATURE_EMBEDDED_SYNTHESIS);
    }

    @Override
    public List<Voice> onGetVoices() {
        List<Voice> voices = new ArrayList<>();
        for (VoiceCatalog.Entry e : catalog(true).entries()) {
            voices.add(new Voice(e.name, e.locale, Voice.QUALITY_NORMAL, Voice.LATENCY_NORMAL,
                    false /* requiresNetworkConnection */,
                    Collections.singleton(TextToSpeech.Engine.KEY_FEATURE_EMBEDDED_SYNTHESIS)));
        }
        return voices;
    }

    @Override
    public String onGetDefaultVoiceNameFor(String lang, String country, String variant) {
        VoiceCatalog.Entry e = catalog(false).defaultFor(lang, country, variant,
                Locale.getDefault().getCountry());
        return e == null ? null : e.name;
    }

    @Override
    public int onIsValidVoiceName(String voiceName) {
        return catalog(false).byName(voiceName) != null ? TextToSpeech.SUCCESS : TextToSpeech.ERROR;
    }

    @Override
    public int onLoadVoice(String voiceName) {
        VoiceCatalog.Entry e = catalog(false).byName(voiceName);
        if (e == null) return TextToSpeech.ERROR;
        mLanguage = new String[] {e.iso3Language, e.iso3Country, ""};
        return TextToSpeech.SUCCESS;
    }

    @Override
    protected void onStop() {
        synchronized (mLock) {
            if (mCurrent != null) mCurrent.cancel();
        }
    }

    @Override
    protected void onSynthesizeText(SynthesisRequest request, SynthesisCallback callback) {
        VoiceCatalog catalog = catalog(false);
        VoiceCatalog.Entry voice = catalog.byName(request.getVoiceName());
        if (voice == null) {
            voice = catalog.defaultFor(request.getLanguage(), request.getCountry(),
                    request.getVariant(), Locale.getDefault().getCountry());
        }
        if (voice == null) {
            // Windows has no voice for the requested language.
            Log.w(TAG, "no Windows voice for " + request.getLanguage() + "-" + request.getCountry());
            fail(callback, TextToSpeech.ERROR_INVALID_REQUEST);
            return;
        }
        CharSequence chars = request.getCharSequenceText();
        String text = chars != null ? chars.toString() : request.getText();
        if (text == null) text = "";
        // Android's rate and pitch are percentages of normal (TextToSpeech.Engine
        // DEFAULT_RATE/DEFAULT_PITCH 100); Windows' are factors of normal, within its ranges.
        double rate = clamp(request.getSpeechRate() / 100.0, SpeechBackend.MIN_RATE,
                SpeechBackend.MAX_RATE);
        double pitch = clamp(request.getPitch() / 100.0, SpeechBackend.MIN_PITCH,
                SpeechBackend.MAX_PITCH);

        SpeechBackend.Synthesis synthesis;
        try {
            synthesis = mBackend.start(text, voice.voice.id, rate, pitch, true);
        } catch (SpeechBackend.StartFailure f) {
            Log.w(TAG, "synthesis did not start: " + f.getMessage());
            if (f.reason == SpeechBackend.REASON_NO_SUCH_VOICE) catalog(true);  // removed
            fail(callback, f.reason == SpeechBackend.REASON_PROSODY_UNSUPPORTED
                    ? TextToSpeech.ERROR_INVALID_REQUEST : TextToSpeech.ERROR_SYNTHESIS);
            return;
        }
        int status;
        byte[] audio = null;
        long[] words = null;
        synchronized (mLock) {
            mCurrent = synthesis;
        }
        try {
            status = synthesis.await(SYNTHESIS_TIMEOUT_MS);
            if (status == SpeechBackend.DONE) {
                audio = synthesis.audio();
                words = synthesis.words();
            }
        } finally {
            synchronized (mLock) {
                mCurrent = null;
            }
            synthesis.release();
        }
        if (status == SpeechBackend.CANCELED) {
            // onStop: the callback is stopped already, and reports onStop to the client.
            return;
        }
        if (status != SpeechBackend.DONE) {
            fail(callback, TextToSpeech.ERROR_SYNTHESIS);
            return;
        }
        WavPcm pcm;
        try {
            pcm = WavPcm.parse(audio);
        } catch (IllegalArgumentException e) {
            Log.w(TAG, "unusable Windows audio: " + e.getMessage());
            fail(callback, TextToSpeech.ERROR_SYNTHESIS);
            return;
        }
        deliver(callback, pcm, audio, ranges(words, text.length(), pcm));
    }

    private static void fail(SynthesisCallback callback, int error) {
        callback.error(error);
        callback.done();
    }

    /** [frame, start, end (exclusive)] of each word inside the text, in frame order. */
    static int[][] ranges(long[] words, int textLength, WavPcm pcm) {
        List<int[]> ranges = new ArrayList<>();
        long frames = pcm.frames();
        for (int i = 0; words != null && i + 2 < words.length; i += 3) {
            long ticks = words[i], first = words[i + 1], last = words[i + 2];
            if (ticks < 0 || first < 0 || last < first || last >= textLength) continue;
            long frame = Math.min(frames, ticks * pcm.sampleRate / 10_000_000L);
            ranges.add(new int[] {(int) frame, (int) first, (int) last + 1});
        }
        ranges.sort((a, b) -> Integer.compare(a[0], b[0]));
        return ranges.toArray(new int[0][]);
    }

    /** start, then each word's rangeStart before the audio that contains it, then done. */
    private static void deliver(SynthesisCallback callback, WavPcm pcm, byte[] audio,
            int[][] ranges) {
        if (callback.start(pcm.sampleRate, pcm.encoding, pcm.channels) != TextToSpeech.SUCCESS) {
            // Stopped (done answers it without a dispatch) or refused (done reports it).
            callback.done();
            return;
        }
        int max = callback.getMaxBufferSize() / pcm.frameSize * pcm.frameSize;
        if (max <= 0) max = pcm.frameSize;
        int next = 0;
        int frame = 0;
        boolean delivered = true;
        for (int offset = 0; offset < pcm.dataLength; ) {
            int length = Math.min(max, pcm.dataLength - offset);
            int end = frame + length / pcm.frameSize;
            while (next < ranges.length && ranges[next][0] < end) {
                int[] r = ranges[next++];
                callback.rangeStart(r[0], r[1], r[2]);
            }
            if (callback.audioAvailable(audio, pcm.dataOffset + offset, length)
                    != TextToSpeech.SUCCESS) {
                delivered = false;  // stopped or failed: done() reports which
                break;
            }
            offset += length;
            frame = end;
        }
        // Words Windows placed at the very end of the audio.
        while (delivered && next < ranges.length && ranges[next][0] <= frame) {
            int[] r = ranges[next++];
            callback.rangeStart(r[0], r[1], r[2]);
        }
        callback.done();
    }

    private static double clamp(double value, double min, double max) {
        return Math.max(min, Math.min(max, value));
    }
}
