/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.app.webview;

import android.os.Handler;
import android.os.HandlerThread;
import android.webkit.WebMessage;
import android.webkit.WebMessagePort;
import org.json.JSONArray;
import org.json.JSONObject;
import java.lang.reflect.Method;
import java.util.Arrays;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

/** Production page-message ingress, endpoint graph and payloads; declared host boundary. */
public final class PageOriginTransferTest {
    private static int checks, failures;
    private static HandlerThread thread;
    private static Handler handler;
    private static int nextId;

    private static void check(boolean ok, String label) {
        checks++;
        if (!ok) { failures++; throw new AssertionError(label); }
    }
    private static String id() { return "page:0123456789abcdef0123456789abcdef:" + (++nextId); }
    private static void ingress(GesherWebViewProvider page, JSONObject message) throws Exception {
        Method method = GesherWebViewProvider.class.getDeclaredMethod("webMessage", String.class, String.class);
        method.setAccessible(true);
        method.invoke(page, message.toString(), "https://example.test/");
    }
    private static JSONObject message(String parent, String text, String... ports) throws Exception {
        return new JSONObject().put("gesherPort", parent).put("data", text)
                .put("ports", new JSONArray(Arrays.asList(ports)));
    }
    private static JSONObject last(GesherWebViewProvider page) throws Exception {
        return new JSONObject(page.mHost.sent.get(page.mHost.sent.size() - 1));
    }
    private static final class Inbox extends WebMessagePort.WebMessageCallback {
        final LinkedBlockingQueue<WebMessage> messages = new LinkedBlockingQueue<>();
        @Override public void onMessage(WebMessagePort port, WebMessage message) { messages.add(message); }
        WebMessage take() throws Exception {
            WebMessage value = messages.poll(2, TimeUnit.SECONDS);
            check(value != null, "callback receives the queued message");
            return value;
        }
    }
    private static Inbox listen(WebMessagePort port) {
        Inbox inbox = new Inbox();
        port.setWebMessageCallback(inbox, handler);
        return inbox;
    }
    private static WebMessagePort[] carrier(GesherWebViewProvider page) {
        WebMessagePort[] pair = GesherWebMessagePort.pair();
        page.postMessageToMainFrame(new WebMessage("carrier", new WebMessagePort[]{pair[1]}), null);
        return pair;
    }
    private static String parent(WebMessagePort[] carrier) { return ((GesherWebMessagePort) carrier[1]).mId; }

    private static void roundTripAndNestedQueue() throws Exception {
        GesherWebViewProvider page = new GesherWebViewProvider();
        WebMessagePort[] carrier = carrier(page);
        Inbox root = listen(carrier[0]);
        String childId = id(), otherId = id(), nestedId = id();
        ingress(page, message(parent(carrier), "children", childId, otherId));
        WebMessage received = root.take();
        check("children".equals(received.getData()), "parent payload retained");
        check(received.getPorts() != null && received.getPorts().length == 2, "page transfers both endpoints in order");
        WebMessagePort child = received.getPorts()[0], other = received.getPorts()[1];
        child.postMessage(new WebMessage("to first"));
        check(childId.equals(last(page).getString("id")), "first received endpoint routes to its page handle");
        other.postMessage(new WebMessage("to second"));
        check(otherId.equals(last(page).getString("id")), "second endpoint order is preserved");
        ingress(page, message(childId, "queued before callback"));
        ingress(page, message(childId, "AP+A", nestedId).put("binary", true));
        Inbox childInbox = listen(child);
        check("queued before callback".equals(childInbox.take().getData()), "received endpoint queues before callback");
        WebMessage binary = childInbox.take();
        check(Arrays.equals(WebMessagePayload.bytes(binary), new byte[]{0, (byte) 255, (byte) 128}), "binary bytes survive page transfer");
        check(binary.getPorts() != null && binary.getPorts().length == 1, "binary message preserves nested endpoint");
        WebMessagePort nested = binary.getPorts()[0];
        nested.postMessage(new WebMessagePayload(new byte[]{4, 5, 6}, null));
        check(nestedId.equals(last(page).getString("id")) && last(page).getBoolean("binary")
                && "BAUG".equals(last(page).getString("data")), "nested endpoint sends typed bytes back to page");
        other.close();
        check(!page.mPagePorts.containsKey(otherId), "app close removes the received endpoint's transport");
        check("close".equals(last(page).getString("gesher")) && otherId.equals(last(page).getString("id")), "app close reaches the native page endpoint");
        nested.close(); child.close(); carrier[0].close();
        check(page.mPagePorts.isEmpty(), "closing app ownership releases all page registrations");
        page.destroy();
    }

    private static void transferToAnotherPage() throws Exception {
        GesherWebViewProvider first = new GesherWebViewProvider(), second = new GesherWebViewProvider();
        WebMessagePort[] carrier = carrier(first);
        Inbox inbox = listen(carrier[0]);
        String originId = id();
        ingress(first, message(parent(carrier), "transfer", originId));
        WebMessagePort child = inbox.take().getPorts()[0];
        second.postMessageToMainFrame(new WebMessage("move", new WebMessagePort[]{child}), null);
        String targetId = last(second).getJSONArray("ports").getString(0);
        try { child.postMessage(new WebMessage("detached")); check(false, "transferred Java wrapper detached"); }
        catch (IllegalStateException expected) { checks++; }
        ingress(first, message(originId, "across pages"));
        check(targetId.equals(last(second).getString("id")) && "across pages".equals(last(second).getString("data")), "page-originated endpoint can move to another WebView");
        ingress(second, message(targetId, "back across"));
        check(originId.equals(last(first).getString("id")) && "back across".equals(last(first).getString("data")), "cross-page bridge remains bidirectional");
        first.destroy();
        int sent = second.mHost.sent.size();
        ingress(second, message(targetId, "old document gone"));
        check(second.mHost.sent.size() == sent, "retired source document is not resurrected");
        carrier[0].close(); second.destroy();
        check(first.mPagePorts.isEmpty() && second.mPagePorts.isEmpty(), "both document registries retire");
    }

    private static void invalidOffersAreAtomic() throws Exception {
        GesherWebViewProvider page = new GesherWebViewProvider();
        WebMessagePort[] carrier = carrier(page);
        Inbox inbox = listen(carrier[0]);
        String duplicate = id();
        int before = page.mHost.sent.size();
        ingress(page, message(parent(carrier), "duplicate", duplicate, duplicate));
        check(page.mPagePorts.size() == 1, "duplicate offer registers no endpoints");
        check(page.mHost.sent.size() == before + 1 && duplicate.equals(last(page).getString("id")), "duplicate orphan transport is closed once");
        String live = id();
        ingress(page, message(parent(carrier), "valid", live));
        WebMessage accepted = inbox.take();
        check("valid".equals(accepted.getData()), "invalid offer delivered no callback");
        WebMessagePort owner = accepted.getPorts()[0];
        String fresh = id();
        ingress(page, message(parent(carrier), "collision", fresh, live));
        check(page.mPagePorts.size() == 2 && !page.mPagePorts.containsKey(fresh), "collision rejects the complete batch");
        check(fresh.equals(last(page).getString("id")), "collision cleanup releases only the unowned offer");
        owner.postMessage(new WebMessage("live owner"));
        check(live.equals(last(page).getString("id")) && "port".equals(last(page).getString("gesher")), "collision cannot close an existing owner");
        String malformed = id();
        ingress(page, message(parent(carrier), "A", malformed).put("binary", true));
        check(page.mPagePorts.size() == 2 && malformed.equals(last(page).getString("id")), "malformed payload rejects and closes its unadmitted transfer");
        ingress(page, message(parent(carrier), "after rejection"));
        check("after rejection".equals(inbox.take().getData()), "rejected payloads do not reach the app");
        owner.close(); carrier[0].close(); page.destroy();
    }

    private static void closedRecipientAndNavigation() throws Exception {
        GesherWebViewProvider page = new GesherWebViewProvider();
        WebMessagePort[] carrier = carrier(page);
        String child = id();
        ingress(page, message(parent(carrier), "queued ownership", child));
        check(page.mPagePorts.size() == 2, "unread message owns its received endpoint");
        carrier[0].close();
        check(page.mPagePorts.isEmpty(), "closing unread recipient retires transferred endpoint graph");

        WebMessagePort[] nextCarrier = carrier(page);
        Inbox inbox = listen(nextCarrier[0]);
        String live = id();
        ingress(page, message(parent(nextCarrier), "before navigation", live));
        WebMessagePort childOwner = inbox.take().getPorts()[0];
        Method retire = GesherWebViewProvider.class.getDeclaredMethod("retirePagePorts");
        retire.setAccessible(true); retire.invoke(page);
        check(page.mPagePorts.isEmpty(), "navigation clears transport registrations");
        String stale = id();
        ingress(page, message(parent(nextCarrier), "late old document", stale));
        check(page.mPagePorts.isEmpty() && stale.equals(last(page).getString("id")), "late ingress releases offered handles without reviving old document");
        int before = page.mHost.sent.size();
        childOwner.postMessage(new WebMessage("peer gone"));
        check(page.mHost.sent.size() == before, "received endpoint cannot send into the next document");
        childOwner.close(); nextCarrier[0].close(); page.destroy();
        ingress(page, message(parent(nextCarrier), "after destroy", id()));
        check(page.mPagePorts.isEmpty(), "destroyed provider admits no transferred endpoints");
    }

    private interface Case { void run() throws Exception; }
    private static void runCase(String name, Case test) {
        try { test.run(); }
        catch (Throwable failure) { failures++; System.err.println(name + ": " + failure); failure.printStackTrace(); }
    }
    public static void main(String[] args) throws Exception {
        thread = new HandlerThread("page-origin-transfer-test"); thread.start();
        handler = new Handler(thread.getLooper());
        try {
            runCase("round trip and nested queue", PageOriginTransferTest::roundTripAndNestedQueue);
            runCase("cross-page transfer", PageOriginTransferTest::transferToAnotherPage);
            runCase("atomic validation", PageOriginTransferTest::invalidOffersAreAtomic);
            runCase("close/navigation", PageOriginTransferTest::closedRecipientAndNavigation);
        } finally { thread.quitSafely(); thread.join(2000); }
        System.out.println("Page-origin transfers: " + checks + " checks, " + failures + " failures");
        if (failures != 0) throw new AssertionError("Page-origin transfer failures");
    }
}
