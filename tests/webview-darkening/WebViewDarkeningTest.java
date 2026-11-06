package com.gesher.app.webview;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.function.Consumer;

/** Policy and delayed command execution, including the actual simulated renderer state. */
public final class WebViewDarkeningTest {
    static int checks;
    static final String AUTO = "Emulation.setAutoDarkModeOverride";
    static final String MEDIA = "Emulation.setEmulatedMedia";
    static final class Call {
        final String method, parameters;
        final Consumer<Boolean> complete;
        boolean done;
        Call(String method, String parameters, Consumer<Boolean> complete) {
            this.method = method; this.parameters = parameters; this.complete = complete;
        }
    }
    static final class Endpoint implements WebViewDarkening.Transport {
        final List<Call> calls = new ArrayList<>();
        final List<String> warnings = new ArrayList<>();
        final Map<String, String> renderer = new HashMap<>();
        final WebViewDarkening policy = new WebViewDarkening(this, warnings::add);
        public void call(String method, String parameters, Consumer<Boolean> complete) {
            check(calls.stream().noneMatch(c -> !c.done && c.method.equals(method)), "no overlapping command for " + method);
            calls.add(new Call(method, parameters, complete));
        }
        Call pending(String method) {
            return calls.stream().filter(c -> !c.done && c.method.equals(method)).findFirst().orElseThrow();
        }
        void reply(String method, boolean success) {
            Call call = pending(method);
            call.done = true;
            if (success) renderer.put(method, call.parameters);
            call.complete.accept(success);
        }
        void settle() {
            for (int i = 0; i < calls.size(); i++) if (!calls.get(i).done) reply(calls.get(i).method, true);
        }
    }
    static void check(boolean condition, String label) {
        if (!condition) throw new AssertionError(label);
        checks++;
    }
    public static void main(String[] args) {
        Endpoint dark = new Endpoint();
        dark.policy.apply(false, 36, false);
        check(dark.pending(MEDIA).parameters.contains("\"dark\""), "theme applies while disallowed");
        check(dark.pending(AUTO).parameters.equals("{\"enabled\":false}"), "default false reaches renderer");
        dark.settle();
        dark.policy.apply(false, 36, true);
        int beforeDisable = dark.calls.size();
        dark.policy.apply(false, 36, false);
        check(dark.calls.size() == beforeDisable, "disable waits for delayed enable completion");
        dark.reply(AUTO, true);
        check(dark.renderer.get(AUTO).contains("true"), "deferred enable changed renderer before completion");
        check(dark.pending(AUTO).parameters.contains("false"), "completion drains latest disable");
        dark.reply(AUTO, true);
        check(dark.renderer.get(AUTO).contains("false"), "final renderer disabled after delayed enable");
        int settled = dark.calls.size();
        dark.policy.apply(false, 36, false);
        check(dark.calls.size() == settled, "same applied value coalesced");
        dark.policy.apply(false, 36, true);
        int oneEnable = dark.calls.size();
        dark.policy.apply(false, 36, false);
        dark.policy.apply(false, 36, true);
        dark.reply(AUTO, true);
        check(dark.calls.size() == oneEnable && dark.renderer.get(AUTO).contains("true"), "latest value coalesces intermediate disable");
        Endpoint light = new Endpoint();
        light.policy.apply(true, 36, true);
        check(light.pending(AUTO).parameters.contains("false"), "light app never algorithmically darkens");
        check(light.pending(MEDIA).parameters.contains("\"light\""), "light media independent of allow");
        light.settle();
        Endpoint legacy = new Endpoint();
        legacy.policy.apply(false, 32, true);
        check(legacy.pending(AUTO).parameters.contains("false"), "modern API has no legacy target effect");
        check(legacy.pending(MEDIA).parameters.contains("\"dark\""), "legacy theme still supplies media");
        legacy.settle();
        check(light.renderer.get(AUTO).contains("false") && dark.renderer.get(AUTO).contains("true"), "independent WebView targets");
        dark.policy.apply(false, 36, false);
        dark.reply(AUTO, false);
        check(dark.warnings.size() == 1, "failed disable reported");
        dark.policy.apply(false, 36, false);
        check(dark.pending(AUTO).parameters.contains("false"), "same-value update retries failed disable");
        dark.reply(AUTO, false);
        check(dark.warnings.size() == 1, "warning bounded per view/control");
        dark.policy.apply(false, 36, false);
        dark.reply(AUTO, true);
        check(dark.renderer.get(AUTO).contains("false"), "retry actually disables renderer");
        Endpoint theme = new Endpoint();
        theme.policy.apply(false, 36, true);
        theme.policy.apply(true, 36, true);
        theme.reply(MEDIA, true);
        check(theme.pending(MEDIA).parameters.contains("\"light\""), "delayed theme drains latest media");
        theme.reply(AUTO, true);
        check(theme.pending(AUTO).parameters.contains("false"), "delayed darkening drains light theme disable");
        theme.settle();
        check(theme.renderer.get(MEDIA).contains("\"light\"") && theme.renderer.get(AUTO).contains("false"), "final theme and renderer agree");
        System.out.println("WebViewDarkeningTest all passed: " + checks);
    }
}
