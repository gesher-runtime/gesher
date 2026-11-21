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
        boolean failImmediately;
        boolean alive = true;
        @Override public boolean isBinderAlive() { return alive; }
        @Override public void createSession(ComponentName component, IBinder token,
                boolean onDevice, IRecognitionServiceManagerCallback callback)
                throws android.os.RemoteException {
            if (failImmediately) throw new android.os.RemoteException("Immediate fixture failure");
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
        boolean alive = true;
        @Override public boolean isBinderAlive() { return alive; }
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

    private static void queueRequests(SpeechRecognizer recognizer, Intent input,
            List<Integer> support, List<Integer> model) {
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
    }

    private static void sessionDeath(Handler main, Manager manager, Context context,
            ComponentName component, Intent input) {
        SpeechRecognizer disconnected = SpeechRecognizer.createSpeechRecognizer(context, component);
        Listener unrelated = new Listener();
        disconnected.setRecognitionListener(unrelated);
        List<Integer> support = new ArrayList<>();
        List<Integer> model = new ArrayList<>();
        queueRequests(disconnected, input, support, model);
        int slot = manager.admissions.size() - 1;
        Engine dead = new Engine();
        manager.answer(slot, dead);
        main.post(() -> {
            // Admission is accepted, but the endpoint dies before the queued commands run.
            dead.alive = false;
            main.post(() -> {
                check(support.equals(List.of(FAILED)) && model.equals(List.of(FAILED))
                        && unrelated.errors.isEmpty(),
                        "commands fail on their own callbacks when a published session dies");
                disconnected.startListening(input);
                check(manager.admissions.size() == slot + 2,
                        "a new operation reconnects after the session dies");
                Engine live = new Engine();
                manager.answer(slot + 1, live);
                main.post(() -> main.post(() -> {
                    check(live.listening == 1, "new connection handles the next operation");
                    disconnected.destroy();
                    main.post(() -> {
                        check(live.shutdowns == 1, "new session is released on destroy");
                        immediateFailure(main, manager, context, component, input);
                    });
                }));
            });
        });
    }

    private static void immediateFailure(Handler main, Manager manager, Context context,
            ComponentName component, Intent input) {
        manager.failImmediately = true;
        Listener before = new Listener(), after = new Listener();
        SpeechRecognizer replaced = SpeechRecognizer.createSpeechRecognizer(context, component);
        replaced.setRecognitionListener(before);
        replaced.setRecognitionListener(after);
        replaced.startListening(input);

        Listener earlier = new Listener(), later = new Listener();
        SpeechRecognizer ordered = SpeechRecognizer.createSpeechRecognizer(context, component);
        ordered.setRecognitionListener(earlier);
        ordered.startListening(input);
        ordered.setRecognitionListener(later);
        ordered.startListening(input);

        Listener canceled = new Listener(), destroyed = new Listener();
        SpeechRecognizer canceling = SpeechRecognizer.createSpeechRecognizer(context, component);
        canceling.setRecognitionListener(canceled);
        canceling.startListening(input);
        canceling.cancel();
        SpeechRecognizer destroying = SpeechRecognizer.createSpeechRecognizer(context, component);
        destroying.setRecognitionListener(destroyed);
        destroying.startListening(input);
        destroying.destroy();

        Listener unrelated = new Listener();
        List<Integer> support = new ArrayList<>(), model = new ArrayList<>();
        SpeechRecognizer commands = SpeechRecognizer.createSpeechRecognizer(context, component);
        commands.setRecognitionListener(unrelated);
        queueRequests(commands, input, support, model);

        manager.failImmediately = false;
        manager.alive = false;
        Listener unavailableBefore = new Listener(), unavailableAfter = new Listener();
        SpeechRecognizer unavailable = SpeechRecognizer.createSpeechRecognizer(context, component);
        unavailable.setRecognitionListener(unavailableBefore);
        unavailable.setRecognitionListener(unavailableAfter);
        unavailable.startListening(input);
        manager.alive = true;

        main.post(() -> main.post(() -> {
            check(before.errors.isEmpty() && after.errors.equals(List.of(FAILED)),
                    "immediate failure uses the preceding listener replacement");
            check(earlier.errors.equals(List.of(FAILED)) && later.errors.equals(List.of(FAILED)),
                    "immediate failure preserves each queued command's listener");
            check(canceled.errors.isEmpty(), "cancel retires a start before immediate failure dispatch");
            check(destroyed.errors.isEmpty(), "destroy retires an immediate failure generation");
            check(support.equals(List.of(FAILED)) && model.equals(List.of(FAILED))
                    && unrelated.errors.isEmpty(), "immediate setup failure uses command-specific callbacks");
            check(unavailableBefore.errors.isEmpty() && unavailableAfter.errors.equals(List.of(FAILED)),
                    "unavailable manager uses the preceding listener replacement");
            replaced.destroy();
            ordered.destroy();
            canceling.destroy();
            commands.destroy();
            unavailable.destroy();
            main.post(() -> { throw new EndLoop(); });
        }));
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
        Listener replacement = new Listener();
        List<Integer> support = new ArrayList<>();
        List<Integer> model = new ArrayList<>();
        SpeechRecognizer recognizer = SpeechRecognizer.createSpeechRecognizer(context, component);
        recognizer.setRecognitionListener(listener);
        recognizer.startListening(input);
        recognizer.setRecognitionListener(replacement);
        recognizer.startListening(input);
        queueRequests(recognizer, input, support, model);
        check(manager.admissions.size() == 1, "queued operations share initial binding");
        manager.answer(0, null);
        main.post(() -> main.post(() -> {
            check(listener.errors.equals(List.of(FAILED))
                    && replacement.errors.equals(List.of(FAILED)),
                    "each start fails on the listener selected when it was queued");
            check(support.equals(List.of(FAILED))
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
                            sessionDeath(main, manager, context, component, input);
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
