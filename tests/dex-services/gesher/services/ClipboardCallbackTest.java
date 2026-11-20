/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.content.ClipData;
import android.os.Binder;
import android.os.RemoteCallbackList;
import com.gesher.server.WindowsHost;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import static gesher.services.ClipboardFixture.*;

/** Actual callback list, caller policy and lifetime ownership; no global production overrides. */
public final class ClipboardCallbackTest {
    private static int checks;
    private static synchronized void check(boolean value, String why) { if (!value) throw new AssertionError(why); checks++; }
    private static void await(CountDownLatch latch, String why) {
        try { check(latch.await(5, TimeUnit.SECONDS), why); }
        catch (InterruptedException failure) { Thread.currentThread().interrupt(); throw new AssertionError(failure); }
    }
    private static void write(ClipboardFixture h, String value) {
        try { h.write(value); } catch (Exception failure) { throw new AssertionError(failure); }
    }
    public static int contracts(boolean binder) throws Exception {
        checks = 0;
        try (ClipboardFixture h = new ClipboardFixture(binder)) {
            RemoteCallbackList<?> list = h.listeners();
            Object clipboardLock = get(h.service, "mClipboardLock");
            int serverPid = Binder.getCallingPid(), serverUid = Binder.getCallingUid();
            Listener nested = new Listener();
            nested.action = () -> {
                check(!Thread.holdsLock(h.service) && !Thread.holdsLock(list) && !Thread.holdsLock(clipboardLock),
                        "Callback ran with a service/list/data monitor held");
                check(Binder.getCallingPid() == serverPid && Binder.getCallingUid() == serverUid,
                        "Listener dispatch retained an app caller's Binder identity");
                if (nested.calls.get() == 1) write(h, "nested change");
            };
            h.add(h.owner, nested); h.write("first change");
            check(nested.calls.get() == 2, "Reentrant clipboard update lost its callback"); h.remove(h.owner, nested);
        }
        try (ClipboardFixture h = new ClipboardFixture(binder)) {
            CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1);
            AtomicReference<Throwable> failed = new AtomicReference<>();
            Listener paused = new Listener();
            paused.action = () -> { if (paused.calls.get() == 1) { entered.countDown(); await(release, "First callback not released"); } };
            h.add(h.owner, paused);
            Thread first = new Thread(() -> { try { h.write("thread change"); } catch (Throwable failure) { failed.set(failure); } }, "clipboard-callback-fixture");
            first.start();
            try { await(entered, "First callback never entered"); h.write("concurrent change"); }
            finally { release.countDown(); first.join(5000); }
            check(!first.isAlive() && failed.get() == null && paused.calls.get() == 2,
                    "Concurrent callback snapshot failed: " + failed.get()); h.remove(h.owner, paused);
        }
        try (ClipboardFixture h = new ClipboardFixture(binder)) {
            Listener added = new Listener(), mutating = new Listener();
            mutating.action = () -> {
                try { h.remove(h.owner, mutating); h.add(h.owner, added); }
                catch (Exception failure) { throw new AssertionError(failure); }
            };
            h.add(h.owner, mutating); h.write("mutation");
            check(mutating.calls.get() == 1 && added.calls.get() == 0, "Mutation changed an in-flight snapshot");
            h.write("next snapshot"); check(mutating.calls.get() == 1 && added.calls.get() == 1, "Mutation missing from next snapshot");
            h.remove(h.owner, added);
        }
        try (ClipboardFixture h = new ClipboardFixture(binder)) {
            Listener broken = new Listener(), healthy = new Listener(), dead = new Listener();
            broken.action = () -> { throw new IllegalStateException("Expected local listener failure"); };
            h.add(h.owner, broken); h.add(h.owner, healthy); h.add(h.owner, dead); dead.token.die();
            h.write("first isolated change"); h.write("second isolated change");
            check(broken.calls.get() == 2 && healthy.calls.get() == 2 && dead.calls.get() == 0, "Dead/throwing callback suppressed a live listener");
            h.remove(h.owner, broken); h.remove(h.owner, healthy);
            check(h.listeners().getRegisteredCallbackCount() == 0 && healthy.token.deaths.isEmpty(), "Callback cleanup leaked registrations");
        }
        return checks;
    }
    /** Separate Windows/ART gate uses the public production constructor and actual native event delivery. */
    private static void windowsClipboard() throws Exception {
        check(WindowsHost.available(), "Windows clipboard backend unavailable");
        String originalText = WindowsHost.clipboardText(), originalHtml = WindowsHost.clipboardHtml();
        byte[] originalImage = WindowsHost.clipboardImage();
        try (ClipboardFixture h = new ClipboardFixture(true, true)) {
            String text = "gesher-clipboard-" + System.nanoTime();
            h.as(h.privileged, () -> h.api.setPrimaryClip(ClipData.newHtmlText("fixture", text, "<b>" + text + "</b>"), PRIVILEGED, "clipboard", 0, 0));
            check(text.equals(WindowsHost.clipboardText()), "Service data did not reach native Windows clipboard");
            if (WindowsHost.sessionLockState() == 0) h.as(h.privileged, () ->
                    check(PRIVILEGED.equals(h.api.getPrimaryClipSource(PRIVILEGED, null, 0, 0)),
                            "Native publication sequence must preserve the Android source until an external change"));
            CountDownLatch changed = new CountDownLatch(1);
            Listener listener = new Listener(); listener.action = changed::countDown; h.add(h.privileged, listener);
            String external = text + "-external";
            check(WindowsHost.setClipboard(external, null), "Native external write failed");
            await(changed, "Native clipboard event was not delivered");
            int lock = WindowsHost.sessionLockState();
            check(lock == -1 || lock == 0 || lock == 1, "Native lock query returned an invalid state");
            h.as(h.privileged, () -> {
                ClipData actual = h.api.getPrimaryClip(PRIVILEGED, "clipboard", 0, 0);
                check(lock == 0 ? actual != null && external.contentEquals(actual.getItemAt(0).getText()) : actual == null,
                        "Native getter failed to respect actual session lock state");
                h.api.clearPrimaryClip(PRIVILEGED, "clipboard", 0, 0);
            });
            check(WindowsHost.clipboardText() == null && WindowsHost.clipboardHtml() == null && !WindowsHost.hasClipboardImage(),
                    "Clear must empty the native clipboard, including the text format");
            h.remove(h.privileged, listener);
        } finally {
            check(originalImage == null ? WindowsHost.setClipboard(originalText, originalHtml)
                    : WindowsHost.setClipboardRich(originalText, originalHtml, originalImage), "Native clipboard restoration failed");
        }
    }
    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime"); contracts(true); windowsClipboard();
        System.out.println("ClipboardCallbackTest all passed: " + checks + " callback and native clipboard checks");
    }
}
