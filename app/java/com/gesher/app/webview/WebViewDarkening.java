package com.gesher.app.webview;

import java.util.function.Consumer;
import java.util.function.Function;

/** Per-WebView Chromium rendering controls; never changes the document or browser profile. */
final class WebViewDarkening {
    interface Transport {
        void call(String method, String parameters, Consumer<Boolean> completed);
    }

    private final Transport mTransport;
    private final Consumer<String> mWarning;
    private final Control mMedia;
    private final Control mDarkening;

    WebViewDarkening(Transport transport, Consumer<String> warning) {
        mTransport = transport;
        mWarning = warning;
        mMedia = new Control("Emulation.setEmulatedMedia", light ->
                "{\"features\":[{\"name\":\"prefers-color-scheme\",\"value\":\""
                        + (light ? "light" : "dark") + "\"}]}", "the app's color scheme");
        mDarkening = new Control("Emulation.setAutoDarkModeOverride", enabled ->
                "{\"enabled\":" + enabled + "}", "algorithmic darkening");
    }

    void apply(boolean lightTheme, int targetSdk, boolean allowed) {
        // Android's prefers-color-scheme follows isLightTheme independently of allow.
        mMedia.set(lightTheme);
        // The modern API has no effect for targetSdk < 33, or in a light app theme.
        boolean enabled = targetSdk >= 33 && !lightTheme && allowed;
        mDarkening.set(enabled);
    }

    /** CDP can process overlapping calls out of order: one outstanding call per control. */
    private final class Control {
        final String method;
        final Function<Boolean, String> parameters;
        final String description;
        Boolean desired;
        Boolean applied;
        boolean pending;
        boolean warned;

        Control(String method, Function<Boolean, String> parameters, String description) {
            this.method = method;
            this.parameters = parameters;
            this.description = description;
        }

        void set(boolean value) {
            desired = value;
            drain();
        }

        void drain() {
            if (pending || desired == null || desired.equals(applied)) return;
            boolean sent = desired;
            pending = true;
            Consumer<Boolean> completed = success -> {
                pending = false;
                applied = success ? sent : null;
                if (!success && !warned) {
                    warned = true;
                    mWarning.accept("WebView2 could not apply " + description + "; renderer state retained");
                }
                // Coalesce updates while waiting; a failure of an unchanged value is
                // retried only on the next explicit settings/theme update.
                if (desired != sent) drain();
            };
            try {
                mTransport.call(method, parameters.apply(sent), completed);
            } catch (RuntimeException failure) {
                completed.accept(false);
            }
        }
    }
}
