/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.content.ClipData;
import android.content.ClipDescription;
import android.content.Intent;
import android.net.Uri;
import android.os.PersistableBundle;
import com.gesher.server.ClipboardService;
import com.gesher.server.PackageManagerService;
import java.io.File;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.util.List;
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
    private static void typedNativeClipboard(ClipboardFixture h, String nonce) throws Exception {
        check(WindowsHost.sessionLockState() == 0,
                "An actual unlocked Windows session is required to exercise typed clipboard reads");
        Uri uri = Uri.parse("content://fixture.provider/private/" + nonce);
        Intent intent = new Intent(Intent.ACTION_VIEW, uri).putExtra("private-token", nonce);
        ClipDescription description = new ClipDescription("private Android description",
                new String[]{"text/html", "text/uri-list", "text/vnd.android.intent"});
        PersistableBundle extras = new PersistableBundle(); extras.putString("private-token", nonce);
        description.setExtras(extras);
        ClipData typed = new ClipData(description, new ClipData.Item(nonce, "<b>" + nonce + "</b>", intent, uri));
        typed.addItem(new ClipData.Item(new Intent(Intent.ACTION_SEND).putExtra("second-token", nonce)));
        h.as(h.privileged, () -> h.api.setPrimaryClip(typed, PRIVILEGED, null, 0, 0));
        String nativeText = WindowsHost.clipboardText(), nativeHtml = WindowsHost.clipboardHtml();
        check(nativeText != null && nativeHtml != null, "Typed Android write publishes its real text/HTML projection");
        h.as(h.privileged, () -> {
            ClipData own = h.api.getPrimaryClip(PRIVILEGED, null, 0, 0);
            check(own != null && own.getItemCount() == 2 && uri.equals(own.getItemAt(0).getUri())
                    && nonce.equals(own.getItemAt(0).getIntent().getStringExtra("private-token"))
                    && nonce.equals(own.getItemAt(1).getIntent().getStringExtra("second-token")),
                    "Read-only native consumption retains every original URI/Intent item of the accepted Android publication");
            check(nonce.equals(own.getDescription().getExtras().getString("private-token"))
                    && PRIVILEGED.equals(h.api.getPrimaryClipSource(PRIVILEGED, null, 0, 0)),
                    "Private description metadata and authenticated source survive own publication");
        });
        check(WindowsHost.setClipboard(nativeText, nativeHtml), "Actual later native writer publishes the same visible projection");
        h.as(h.privileged, () -> {
            ClipData external = h.api.getPrimaryClip(PRIVILEGED, null, 0, 0);
            check(external != null && external.getItemCount() == 1
                    && external.getItemAt(0).getUri() == null && external.getItemAt(0).getIntent() == null
                    && nativeText.contentEquals(external.getItemAt(0).getText()),
                    "A new equal-looking native generation retires stale private Android URI/Intent data");
            check(h.api.getPrimaryClipSource(PRIVILEGED, null, 0, 0) == null,
                    "A later equal-looking Windows clip never re-adopts the preceding Android source");
        });
        File image = File.createTempFile("clipboard-native-typed-", ".bmp");
        try {
            ByteBuffer bmp = ByteBuffer.allocate(62).order(ByteOrder.LITTLE_ENDIAN);
            bmp.putShort((short)0x4d42).putInt(62).putInt(0).putInt(54);
            bmp.putInt(40).putInt(2).putInt(1).putShort((short)1).putShort((short)32);
            bmp.putInt(0).putInt(8).putInt(3780).putInt(3780).putInt(0).putInt(0);
            bmp.putInt(0x00735231).putInt(0x00dbb997);
            Files.write(image.toPath(), bmp.array());
            Uri imageUri = Uri.fromFile(image);
            ClipData rich = new ClipData(new ClipDescription("original BMP", new String[]{"image/bmp"}),
                    new ClipData.Item(nonce, null, intent, imageUri));
            h.as(h.privileged, () -> h.api.setPrimaryClip(rich, PRIVILEGED, null, 0, 0));
            byte[] nativeImage = WindowsHost.clipboardImage();
            check(nativeImage != null && nativeImage.length > 54, "Actual native preferred image reader receives the requested BMP");
            h.as(h.privileged, () -> {
                ClipData own = h.api.getPrimaryClip(PRIVILEGED, null, 0, 0);
                check(own != null && imageUri.equals(own.getItemAt(0).getUri())
                        && nonce.equals(own.getItemAt(0).getIntent().getStringExtra("private-token"))
                        && PRIVILEGED.equals(h.api.getPrimaryClipSource(PRIVILEGED, null, 0, 0)),
                        "Native rich publication retains its original typed image URI, Intent and source");
            });
            check(!WindowsHost.setClipboardRich("partial text is not the requested image", null, new byte[]{1, 2, 3}),
                    "Invalid rich input reports failure instead of fabricating text-only success");
            check(nonce.equals(WindowsHost.clipboardText()), "Failed rich input preserves the actual accepted native content");
            check(WindowsHost.setClipboardRich(WindowsHost.clipboardText(), null, nativeImage),
                    "Actual native replacement replays the complete visible image projection");
            h.as(h.privileged, () -> {
                ClipData external = h.api.getPrimaryClip(PRIVILEGED, null, 0, 0);
                check(external != null && external.getItemAt(0).getUri() != null
                        && !imageUri.equals(external.getItemAt(0).getUri()) && external.getItemAt(0).getIntent() == null,
                        "A later native image is imported into owned storage without reviving the old typed URI or Intent");
                check(h.api.getPrimaryClipSource(PRIVILEGED, null, 0, 0) == null,
                        "Replayed native image data does not inherit Android source attribution");
                h.api.clearPrimaryClip(PRIVILEGED, null, 0, 0);
            });
        } finally { Files.deleteIfExists(image.toPath()); }
        h.as(h.privileged, () -> h.api.setPrimaryClip(ClipData.newPlainText("empty", ""), PRIVILEGED, null, 0, 0));
        check("".equals(WindowsHost.clipboardText()), "An empty Android text clip remains a present native text value");
        h.as(h.privileged, () -> {
            check(h.api.hasPrimaryClip(PRIVILEGED, null, 0, 0), "Empty text remains an Android primary clip");
            h.api.clearPrimaryClip(PRIVILEGED, null, 0, 0);
            check(!h.api.hasPrimaryClip(PRIVILEGED, null, 0, 0), "True clear removes the primary clip");
        });
        check(WindowsHost.clipboardText() == null && WindowsHost.clipboardHtml() == null
                && !WindowsHost.hasClipboardImage(), "True clear removes native formats after typed image/text use");
    }

    private static int nativeSubscriptions() throws Exception {
        var listeners = WindowsHost.class.getDeclaredField("sListeners"); listeners.setAccessible(true);
        return ((List<?>)listeners.get(null)).size();
    }
    private static void overlappingNativeLifetimes(ClipboardFixture h, String nonce) throws Exception {
        int initial = nativeSubscriptions();
        ClipboardService second = (ClipboardService) make(ClipboardService.class,
                new Class<?>[]{h.atm.getClass(), PackageManagerService.class}, h.atm, h.pm);
        try {
            h.as(h.privileged, () -> second.setPrimaryClip(ClipData.newPlainText("second", nonce), PRIVILEGED, null, 0, 0));
            check(nativeSubscriptions() == initial + 1, "Second live service owns a distinct native subscription");
            h.service.close();
            check(nativeSubscriptions() == initial, "Closing the first service releases only its own subscription");
            h.as(h.privileged, () -> {
                second.setPrimaryClip(ClipData.newPlainText("still live", nonce + "-live"), PRIVILEGED, null, 0, 0);
                ClipData own = second.getPrimaryClip(PRIVILEGED, null, 0, 0);
                check(own != null && (nonce + "-live").contentEquals(own.getItemAt(0).getText())
                        && PRIVILEGED.equals(second.getPrimaryClipSource(PRIVILEGED, null, 0, 0)),
                        "Closing one service does not stop the other service's real native publication");
            });
        } finally { second.close(); }
        check(nativeSubscriptions() == initial - 1, "Last service releases its native subscription");
        check((nonce + "-live").equals(WindowsHost.clipboardText()),
                "Last service retirement preserves its accepted Windows content");
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
            typedNativeClipboard(h, text);
            overlappingNativeLifetimes(h, text);
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
