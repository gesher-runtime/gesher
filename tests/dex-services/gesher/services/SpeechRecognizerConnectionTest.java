/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.content.AttributionSource;
import android.content.ComponentName;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.ServiceManager;
import android.speech.IModelDownloadListener;
import android.speech.IRecognitionListener;
import android.speech.IRecognitionService;
import android.speech.IRecognitionServiceManager;
import android.speech.IRecognitionServiceManagerCallback;
import android.speech.IRecognitionSupportCallback;
import android.speech.ModelDownloadListener;
import android.speech.RecognitionListener;
import android.speech.RecognitionSupport;
import android.speech.RecognitionSupportCallback;
import android.speech.RecognizerIntent;
import android.speech.SpeechRecognizer;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/** Public SpeechRecognizer commands and callbacks driven by the actual main Looper. */
public final class SpeechRecognizerConnectionTest {
    private static final class EndLoop extends RuntimeException {}
    private static final int FAILED = SpeechRecognizer.ERROR_CLIENT;
    private static int checks;

    private static void check(boolean result, String detail) {
        if (!result) throw new AssertionError(detail);
        checks++;
    }

    private static final class Manager extends IRecognitionServiceManager.Stub {
        final List<IRecognitionServiceManagerCallback> admissions = new ArrayList<>();
        final List<IBinder> tokens = new ArrayList<>();
        @Override public void createSession(ComponentName component, IBinder token,
                boolean onDevice, IRecognitionServiceManagerCallback callback) {
            check(component.equals(new ComponentName("fixture.engine", "fixture.engine.Provider"))
                    && !onDevice, "explicit component selected");
            admissions.add(callback);
            tokens.add(token);
        }
        @Override public void setTemporaryComponent(ComponentName component) {
            throw new AssertionError("Unexpected temporary component");
        }
        void answer(int slot, Engine engine) {
            try {
                if (engine == null) admissions.get(slot).onError(FAILED);
                else admissions.get(slot).onSuccess(engine);
            } catch (android.os.RemoteException failure) { throw new AssertionError(failure); }
        }
    }

    private static final class Engine extends IRecognitionService.Stub {
        int listening;
        int shutdowns;
        @Override public void startListening(Intent intent, IRecognitionListener listener,
                AttributionSource source) { listening++; }
        @Override public void stopListening(IRecognitionListener listener) {}
        @Override public void cancel(IRecognitionListener listener, boolean shutdown) {
            if (shutdown) shutdowns++;
        }
        @Override public void checkRecognitionSupport(Intent intent, AttributionSource source,
                IRecognitionSupportCallback callback) {}
        @Override public void triggerModelDownload(Intent intent, AttributionSource source,
                IModelDownloadListener callback) {}
    }

    private static final class Listener implements RecognitionListener {
        final List<Integer> errors = new ArrayList<>();
        @Override public void onReadyForSpeech(Bundle value) {}
        @Override public void onBeginningOfSpeech() {}
        @Override public void onRmsChanged(float value) {}
        @Override public void onBufferReceived(byte[] value) {}
        @Override public void onEndOfSpeech() {}
        @Override public void onError(int value) { errors.add(value); }
        @Override public void onResults(Bundle value) {}
        @Override public void onPartialResults(Bundle value) {}
        @Override public void onEvent(int value, Bundle params) {}
    }

    public static void main(String[] args) {
        if (args.length == 0) System.loadLibrary("android_runtime");
        else for (String library : args) System.load(library);
        if (Looper.myLooper() == null) Looper.prepareMainLooper();
        Handler main = new Handler(Looper.getMainLooper());
        Manager manager = new Manager();
        ServiceManager.initServiceCache(Map.of(Context.SPEECH_RECOGNITION_SERVICE, manager));
        Context context = new ContextWrapper(null) {
            @Override public AttributionSource getAttributionSource() {
                return new AttributionSource.Builder(10123).setPackageName("fixture.client").build();
            }
        };
        ComponentName component = new ComponentName("fixture.engine", "fixture.engine.Provider");
        Intent input = new Intent(RecognizerIntent.ACTION_RECOGNIZE_SPEECH);
        Listener listener = new Listener();
        List<Integer> support = new ArrayList<>();
        List<Integer> model = new ArrayList<>();
        SpeechRecognizer recognizer = SpeechRecognizer.createSpeechRecognizer(context, component);
        recognizer.setRecognitionListener(listener);
        recognizer.startListening(input);
        recognizer.checkRecognitionSupport(input, Runnable::run, new RecognitionSupportCallback() {
            @Override public void onSupportResult(RecognitionSupport result) { throw new AssertionError(); }
            @Override public void onError(int error) { support.add(error); }
        });
        recognizer.triggerModelDownload(input, Runnable::run, new ModelDownloadListener() {
            @Override public void onProgress(int value) { throw new AssertionError(); }
            @Override public void onSuccess() { throw new AssertionError(); }
            @Override public void onScheduled() { throw new AssertionError(); }
            @Override public void onError(int error) { model.add(error); }
        });
        check(manager.admissions.size() == 1, "queued operations share initial binding");
        manager.answer(0, null);
        main.post(() -> main.post(() -> {
            check(listener.errors.equals(List.of(FAILED)) && support.equals(List.of(FAILED))
                    && model.equals(List.of(FAILED)), "each command receives its own admission error");
            recognizer.startListening(input);
            check(manager.admissions.size() == 2 && manager.tokens.get(1) != manager.tokens.get(0),
                    "retry after failure uses a new session generation");
            recognizer.cancel();
            Engine engine = new Engine();
            manager.answer(1, engine);
            main.post(() -> main.post(() -> {
                check(engine.listening == 0, "cancel before publication prevents starting capture");
                recognizer.startListening(input);
                main.post(() -> {
                    check(engine.listening == 1, "subsequent start uses connected provider");
                    recognizer.destroy();
                    main.post(() -> {
                        check(engine.shutdowns == 1, "destroy releases connected endpoint");
                        SpeechRecognizer pending = SpeechRecognizer.createSpeechRecognizer(context, component);
                        pending.startListening(input);
                        pending.destroy();
                        Engine late = new Engine();
                        manager.answer(2, late);
                        main.post(() -> {
                            check(late.shutdowns == 1 && late.listening == 0,
                                    "late bind after destroy shuts down without capturing");
                            throw new EndLoop();
                        });
                    });
                });
            }));
        }));
        main.postDelayed(() -> { throw new AssertionError("SpeechRecognizer contract timed out"); }, 5000);
        try { Looper.loop(); } catch (EndLoop complete) { /* Fresh fixture VM. */ }
        System.out.println("SpeechRecognizerConnectionTest all passed: " + checks + " checks");
    }
}
