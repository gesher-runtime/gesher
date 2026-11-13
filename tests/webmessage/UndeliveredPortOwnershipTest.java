package com.gesher.app.webview;

import android.webkit.WebMessage;
import android.webkit.WebMessagePort;
import java.lang.reflect.Field;
import java.util.ArrayDeque;
import org.json.JSONObject;

/**
 * Host-only ownership observations. Public consumer API coverage remains in the
 * separately SDK-compiled WebMessagePortContractTest. Reflection here observes
 * endpoint lifetime/queues and supplies two internal ingress states (an owned
 * incoming message and a disentangled endpoint); no replacement port is used.
 */
public final class UndeliveredPortOwnershipTest {
    private static int checks, failures;

    private static void check(boolean ok, String label) {
        checks++;
        if (!ok) { failures++; System.err.println("FAIL: " + label); }
    }

    private static Field field(String name) throws Exception {
        Field field = GesherWebMessagePort.class.getDeclaredField(name);
        field.setAccessible(true);
        return field;
    }

    private static GesherWebMessagePort peer(WebMessagePort port) throws Exception {
        return (GesherWebMessagePort) field("mPeer").get(port);
    }

    @SuppressWarnings("unchecked")
    private static ArrayDeque<WebMessage> queue(WebMessagePort port) throws Exception {
        return (ArrayDeque<WebMessage>) field("mQueue").get(port);
    }

    private static boolean retired(WebMessagePort port) throws Exception {
        return port == null || (field("mClosed").getBoolean(port) && queue(port).isEmpty());
    }

    private static void detached(WebMessagePort port, String label) {
        try { port.close(); check(false, label); }
        catch (IllegalStateException expected) { check(true, label); }
    }

    private static void closedDestination(boolean beforeSend) throws Exception {
        WebMessagePort[] carrier = GesherWebMessagePort.pair(), child = GesherWebMessagePort.pair();
        if (beforeSend) carrier[1].close();
        carrier[0].postMessage(new WebMessage("owned", new WebMessagePort[]{child[0]}));
        GesherWebMessagePort owner = peer(child[1]);
        if (!beforeSend) carrier[1].close();
        child[1].postMessage(new WebMessage("after discard"));
        check(retired(owner), (beforeSend ? "closed recipient" : "queued close") + " retires transferred ownership");
        detached(child[0], "discard still consumes sender's endpoint");
        carrier[0].close(); child[1].close();
    }

    private static void nestedQueueOwnership() throws Exception {
        final int depth = 128;
        WebMessagePort[][] chain = new WebMessagePort[depth][];
        for (int i = 0; i < depth; i++) chain[i] = GesherWebMessagePort.pair();
        for (int i = depth - 2; i >= 0; i--) {
            chain[i][0].postMessage(new WebMessage("nested", new WebMessagePort[]{chain[i + 1][1]}));
        }
        GesherWebMessagePort[] owners = new GesherWebMessagePort[depth];
        for (int i = 0; i < depth; i++) owners[i] = peer(chain[i][0]);
        chain[0][1].close();
        boolean allRetired = true;
        for (int i = 0; i < depth; i++) {
            chain[i][0].postMessage(new WebMessage("bounded probe"));
            allRetired &= retired(owners[i]);
            chain[i][0].close();
        }
        check(allRetired, "discard visits every nested queued owner without retaining subsequent messages");
    }

    private static void recipientInTransferList() throws Exception {
        WebMessagePort[] carrier = GesherWebMessagePort.pair(), child = GesherWebMessagePort.pair();
        carrier[0].postMessage(new WebMessage("nested", new WebMessagePort[]{child[0]}));
        GesherWebMessagePort childOwner = peer(child[1]);
        carrier[0].postMessage(new WebMessage("target", new WebMessagePort[]{carrier[1]}));
        check(queue(carrier[1]).isEmpty(), "recipient transfer does not queue on the detached wrapper");
        GesherWebMessagePort recipientOwner = peer(carrier[0]);
        carrier[0].postMessage(new WebMessage("after target transfer"));
        child[1].postMessage(new WebMessage("after nested discard"));
        check(retired(recipientOwner), "recipient transfer consumes and retires its unreachable new owner");
        check(retired(childOwner), "recipient transfer retires ownership in its old queue");
        detached(carrier[1], "recipient wrapper remains detached");
        carrier[0].close(); child[1].close();
    }

    private static void cyclicQueueOwnership() throws Exception {
        WebMessagePort[] first = GesherWebMessagePort.pair(), second = GesherWebMessagePort.pair();
        first[0].postMessage(new WebMessage("second", new WebMessagePort[]{second[0]}));
        second[1].postMessage(new WebMessage("first", new WebMessagePort[]{first[1]}));
        GesherWebMessagePort firstOwner = peer(first[0]), secondOwner = peer(second[1]);
        // These ordinary transfers form a two-node queue ownership cycle. The
        // host lifecycle boundary retires one observed internal root directly.
        firstOwner.pageGone();
        first[0].postMessage(new WebMessage("after cyclic discard"));
        second[1].postMessage(new WebMessage("after cyclic discard"));
        check(retired(firstOwner) && retired(secondOwner), "retirement terminates and empties cyclic queued ownership");
        first[0].close(); second[1].close();
    }

    private static void destroyedPage() throws Exception {
        WebMessagePort[] carrier = GesherWebMessagePort.pair(), child = GesherWebMessagePort.pair();
        GesherWebViewProvider page = new GesherWebViewProvider();
        WebView2Host host = page.mHost;
        page.postMessageToMainFrame(new WebMessage("create", new WebMessagePort[]{carrier[1]}), null);
        page.destroy();
        check(page.mPagePorts.isEmpty(), "destroy clears the old page registry");
        check(retired(carrier[1]), "destroy retires the page-owned endpoint");
        check(host.destroyed && page.mHost == null, "destroy preserves host teardown");
        carrier[0].postMessage(new WebMessage("late", new WebMessagePort[]{child[0]}));
        child[1].postMessage(new WebMessage("after vanished destination"));
        check(retired(peer(child[1])), "posting after destroy disposes the consumed transfer");
        detached(child[0], "posting after destroy detaches original transfer wrapper");
        carrier[0].close(); child[1].close(); page.destroy();
    }

    private static void closeBeforeFlush() throws Exception {
        WebMessagePort[] carrier = GesherWebMessagePort.pair(), child = GesherWebMessagePort.pair();
        carrier[0].postMessage(new WebMessage("queued", new WebMessagePort[]{child[0]}));
        GesherWebMessagePort childOwner = peer(child[1]);
        GesherWebViewProvider page = new GesherWebViewProvider();
        page.mHost.afterSend = text -> {
            try {
                if ("post".equals(new JSONObject(text).getString("gesher"))) carrier[0].close();
            } catch (Exception e) { throw new AssertionError(e); }
        };
        page.postMessageToMainFrame(new WebMessage("create", new WebMessagePort[]{carrier[1]}), null);
        check(page.mPagePorts.isEmpty(), "close during creation prevents nested page registration");
        child[1].postMessage(new WebMessage("after close during creation"));
        check(retired(childOwner), "close before flush retires the still-queued nested endpoint");
        child[1].close(); page.destroy();
    }

    private static void closeDuringFlushSnapshot() throws Exception {
        WebMessagePort[] carrier = GesherWebMessagePort.pair();
        WebMessagePort[] delivered = GesherWebMessagePort.pair(), dropped = GesherWebMessagePort.pair();
        carrier[0].postMessage(new WebMessage("first", new WebMessagePort[]{delivered[0]}));
        carrier[0].postMessage(new WebMessage("second", new WebMessagePort[]{dropped[0]}));
        GesherWebMessagePort deliveredOwner = peer(delivered[1]), droppedOwner = peer(dropped[1]);
        GesherWebViewProvider page = new GesherWebViewProvider();
        final boolean[] backendOutsideLock = {true};
        Object graphLock = field("sLock").get(null);
        page.mHost.afterSend = text -> {
            backendOutsideLock[0] &= !Thread.holdsLock(graphLock);
            try {
                JSONObject event = new JSONObject(text);
                if ("port".equals(event.getString("gesher")) && "first".equals(event.optString("data"))) {
                    carrier[0].close();
                }
            } catch (Exception e) { throw new AssertionError(e); }
        };
        page.postMessageToMainFrame(new WebMessage("create", new WebMessagePort[]{carrier[1]}), null);
        check(page.mPagePorts.size() == 1 && page.mPagePorts.get(deliveredOwner.mId) == deliveredOwner,
                "closing a flush snapshot preserves only the already-delivered page transfer");
        dropped[1].postMessage(new WebMessage("after snapshot discard"));
        check(retired(droppedOwner), "remaining snapshot owns and retires its undelivered endpoint");
        int beforeReply = page.mHost.sent.size();
        delivered[1].postMessage(new WebMessage("delivered endpoint still works"));
        check(page.mHost.sent.size() == beforeReply + 1, "already-delivered page endpoint remains usable");
        delivered[1].close(); dropped[1].close();
        check(page.mPagePorts.isEmpty(), "surviving app peer can release the delivered page endpoint");
        check(backendOutsideLock[0], "normal and reentrant backend emissions never hold the port graph lock");
        page.destroy();
    }

    private static WebMessage incomingOwnedMessage(WebMessagePort[] child) throws Exception {
        WebMessagePort[] ingress = GesherWebMessagePort.pair();
        ingress[0].postMessage(new WebMessage("incoming", new WebMessagePort[]{child[0]}));
        // Simulate the internal ingress boundary taking ownership of one message.
        WebMessage owned = queue(ingress[1]).remove();
        ingress[0].close(); ingress[1].close();
        return owned;
    }

    private static void ignoredPageIngress() throws Exception {
        WebMessagePort[] carrier = GesherWebMessagePort.pair(), child = GesherWebMessagePort.pair();
        GesherWebViewProvider page = new GesherWebViewProvider();
        page.postMessageToMainFrame(new WebMessage("create", new WebMessagePort[]{carrier[1]}), null);
        page.destroy();
        ((GesherWebMessagePort) carrier[1]).fromPage(incomingOwnedMessage(child));
        child[1].postMessage(new WebMessage("after ignored ingress"));
        check(retired(peer(child[1])), "ignored page ingress retires its owned transfer");
        carrier[0].close(); child[1].close();
    }

    private static void movedElsewhereIsPreserved() throws Exception {
        WebMessagePort[] child = GesherWebMessagePort.pair(), carrier = GesherWebMessagePort.pair();
        WebMessage oldMessage = incomingOwnedMessage(child);
        WebMessagePort owner = oldMessage.getPorts()[0];
        GesherWebViewProvider livePage = new GesherWebViewProvider(), oldPage = new GesherWebViewProvider();
        livePage.postMessageToMainFrame(new WebMessage("moved again", new WebMessagePort[]{owner}), null);
        oldPage.postMessageToMainFrame(new WebMessage("old destination", new WebMessagePort[]{carrier[1]}), null);
        oldPage.destroy();
        // An internal stale message reference has no ownership once the endpoint
        // was transferred elsewhere; ignoring it must not close its new owner.
        ((GesherWebMessagePort) carrier[1]).fromPage(oldMessage);
        check(livePage.mPagePorts.size() == 1, "stale discarded message preserves ownership already moved to another page");
        int before = livePage.mHost.sent.size();
        child[1].postMessage(new WebMessage("still owned elsewhere"));
        check(livePage.mHost.sent.size() == before + 1, "separately transferred endpoint remains usable after stale discard");
        child[1].close(); carrier[0].close(); livePage.destroy();
    }

    private static void disconnectedAndUnavailableHost() throws Exception {
        WebMessagePort[] carrier = GesherWebMessagePort.pair(), child = GesherWebMessagePort.pair();
        // A transport-disentangled endpoint still consumes postMessage transfers.
        field("mPeer").set(carrier[0], null);
        carrier[0].postMessage(new WebMessage("disconnected", new WebMessagePort[]{child[0]}));
        child[1].postMessage(new WebMessage("after disconnected discard"));
        check(retired(peer(child[1])), "no-recipient send retires its transferred ownership");
        detached(child[0], "no-recipient send consumes original wrapper");
        carrier[0].close(); carrier[1].close(); child[1].close();

        WebMessagePort[] pageCarrier = GesherWebMessagePort.pair(), pageChild = GesherWebMessagePort.pair();
        GesherWebViewProvider page = new GesherWebViewProvider();
        page.postMessageToMainFrame(new WebMessage("create", new WebMessagePort[]{pageCarrier[1]}), null);
        page.mHost = null;
        pageCarrier[0].postMessage(new WebMessage("no host", new WebMessagePort[]{pageChild[0]}));
        pageChild[1].postMessage(new WebMessage("after host loss"));
        check(retired(peer(pageChild[1])), "unavailable host discards already-consumed transfer ownership");
        detached(pageChild[0], "unavailable host does not leave sender owning a dropped transfer");
        pageCarrier[0].close(); pageChild[1].close(); page.destroy();
    }

    private interface Case { void run() throws Exception; }
    private static void runCase(String name, Case test) {
        try { test.run(); }
        catch (Throwable failure) { check(false, name + ": " + failure); failure.printStackTrace(System.err); }
    }

    public static void main(String[] args) {
        runCase("queued close", () -> closedDestination(false));
        runCase("already closed", () -> closedDestination(true));
        runCase("nested ownership", UndeliveredPortOwnershipTest::nestedQueueOwnership);
        runCase("recipient transfer", UndeliveredPortOwnershipTest::recipientInTransferList);
        runCase("cyclic ownership", UndeliveredPortOwnershipTest::cyclicQueueOwnership);
        runCase("destroy", UndeliveredPortOwnershipTest::destroyedPage);
        runCase("close before flush", UndeliveredPortOwnershipTest::closeBeforeFlush);
        runCase("flush snapshot", UndeliveredPortOwnershipTest::closeDuringFlushSnapshot);
        runCase("ignored ingress", UndeliveredPortOwnershipTest::ignoredPageIngress);
        runCase("ownership moved elsewhere", UndeliveredPortOwnershipTest::movedElsewhereIsPreserved);
        runCase("disconnected", UndeliveredPortOwnershipTest::disconnectedAndUnavailableHost);
        System.out.println("Undelivered port ownership: " + checks + " checks, " + failures + " failures");
        if (failures != 0) throw new AssertionError("Undelivered ownership failures");
    }
}
