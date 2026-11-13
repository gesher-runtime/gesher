package gesher.services;

import android.os.Handler;
import android.os.HandlerThread;
import android.webkit.WebMessage;
import android.webkit.WebMessagePort;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/** Public WebMessagePort calls; pair construction alone uses the provider factory. */
public final class WebMessagePortContractTest {
    private static int checks;
    private static int failures;
    private static Handler handler;

    private static void check(boolean value, String label) {
        checks++;
        if (!value) { failures++; System.err.println("FAIL: " + label); }
    }
    private static void rejected(Runnable call, String label) {
        try { call.run(); check(false, label); }
        catch (IllegalStateException expected) { check(true, label); }
    }
    private static WebMessagePort[] pair() throws Exception {
        Method factory = Class.forName("com.gesher.app.webview.GesherWebMessagePort").getDeclaredMethod("pair");
        factory.setAccessible(true);
        return (WebMessagePort[]) factory.invoke(null);
    }
    private static final class Inbox extends WebMessagePort.WebMessageCallback {
        final List<WebMessage> messages = new ArrayList<>();
        final List<WebMessagePort> targets = new ArrayList<>();
        @Override public synchronized void onMessage(WebMessagePort port, WebMessage message) {
            targets.add(port); messages.add(message); notifyAll();
        }
        synchronized void await(int size) throws Exception {
            long end = System.nanoTime() + TimeUnit.SECONDS.toNanos(4);
            while (messages.size() < size) {
                long remaining = end - System.nanoTime();
                if (remaining <= 0) throw new AssertionError("Missing message " + size);
                TimeUnit.NANOSECONDS.timedWait(this, remaining);
            }
        }
        synchronized WebMessage at(int index) { return messages.get(index); }
        synchronized int size() { return messages.size(); }
    }
    private static Inbox listen(WebMessagePort port) {
        Inbox inbox = new Inbox(); port.setWebMessageCallback(inbox, handler); return inbox;
    }
    private static void queuedDelivery() throws Exception {
        WebMessagePort[] ports = pair();
        ports[0].postMessage(new WebMessage("one"));
        ports[0].postMessage(new WebMessage("two"));
        Inbox inbox = listen(ports[1]);
        ports[0].postMessage(new WebMessage("three"));
        inbox.await(3);
        for (int i = 0; i < 3; i++) {
            check(inbox.at(i).getData().equals(new String[]{"one", "two", "three"}[i]), "FIFO " + i);
            check(inbox.targets.get(i) == ports[1], "callback destination " + i);
        }
        ports[0].close(); ports[1].close();
    }
    private static void localTransfer() throws Exception {
        WebMessagePort[] carrier = pair(), moved = pair();
        Inbox carrierInbox = listen(carrier[1]);
        moved[0].postMessage(new WebMessage("queued before transfer"));
        carrier[0].postMessage(new WebMessage("transfer", new WebMessagePort[]{moved[1]}));
        carrierInbox.await(1);
        WebMessagePort received = carrierInbox.at(0).getPorts()[0];
        check(received != moved[1], "recipient owns a fresh wrapper");
        rejected(() -> moved[1].postMessage(new WebMessage("old")), "detached sender cannot post");
        rejected(() -> moved[1].setWebMessageCallback(null, handler), "detached sender cannot set callback");
        rejected(moved[1]::close, "detached sender cannot close");
        Inbox receivedInbox = listen(received), peerInbox = listen(moved[0]);
        receivedInbox.await(1);
        check("queued before transfer".equals(receivedInbox.at(0).getData()), "queue follows transferred endpoint");
        received.postMessage(new WebMessage("reply")); peerInbox.await(1);
        check("reply".equals(peerInbox.at(0).getData()), "recipient can reply to original peer");
        moved[0].postMessage(new WebMessage("new")); receivedInbox.await(2);
        check("new".equals(receivedInbox.at(1).getData()), "peer follows the new owner");
        carrier[0].close(); carrier[1].close(); received.close(); moved[0].close();
    }
    private static void transferBothAndAgain() throws Exception {
        WebMessagePort[] carrier = pair(), both = pair(), secondCarrier = pair();
        Inbox first = listen(carrier[1]), second = listen(secondCarrier[1]);
        carrier[0].postMessage(new WebMessage("both", both)); first.await(1);
        WebMessagePort[] received = first.at(0).getPorts();
        check(received[0] != both[0] && received[1] != both[1], "both endpoints get new owners");
        secondCarrier[0].postMessage(new WebMessage("again", received)); second.await(1);
        WebMessagePort[] finalPorts = second.at(0).getPorts();
        Inbox inbox = listen(finalPorts[1]); finalPorts[0].postMessage(new WebMessage("entangled"));
        inbox.await(1); check("entangled".equals(inbox.at(0).getData()), "both endpoints remain entangled after two transfers");
        rejected(() -> received[0].postMessage(new WebMessage("old")), "intermediate owner detached");
        for (WebMessagePort port : new WebMessagePort[]{carrier[0],carrier[1],secondCarrier[0],secondCarrier[1],finalPorts[0],finalPorts[1]}) port.close();
    }
    private static void transferRestrictions() throws Exception {
        WebMessagePort[] carrier = pair(), posted = pair(), callback = pair(), closed = pair(), fresh = pair();
        posted[0].postMessage(new WebMessage("started"));
        callback[0].setWebMessageCallback(null, handler);
        closed[0].close();
        rejected(() -> carrier[0].postMessage(new WebMessage("used", new WebMessagePort[]{posted[0]})), "posted port cannot transfer");
        rejected(() -> carrier[0].postMessage(new WebMessage("callback", new WebMessagePort[]{callback[0]})), "callback registration starts port even when null");
        rejected(() -> carrier[0].postMessage(new WebMessage("closed", new WebMessagePort[]{closed[0]})), "closed port cannot transfer");
        rejected(() -> carrier[0].postMessage(new WebMessage("self", new WebMessagePort[]{carrier[0]})), "source cannot transfer itself");
        rejected(() -> carrier[0].postMessage(new WebMessage("batch", new WebMessagePort[]{fresh[0], posted[0]})), "whole transfer batch validated");
        Inbox carrierInbox = listen(carrier[1]);
        carrier[0].postMessage(new WebMessage("still owned", new WebMessagePort[]{fresh[0]})); carrierInbox.await(1);
        WebMessagePort accepted = carrierInbox.at(0).getPorts()[0];
        Inbox freshPeer = listen(fresh[1]); accepted.postMessage(new WebMessage("kept")); freshPeer.await(1);
        check("kept".equals(freshPeer.at(0).getData()), "rejected batch does not transfer an earlier element");
        WebMessagePort[] duplicate = pair();
        rejected(() -> carrier[0].postMessage(new WebMessage("duplicate", new WebMessagePort[]{duplicate[0], duplicate[0]})), "duplicate transfer rejected");
        duplicate[0].postMessage(new WebMessage("usable after rejection"));
        Inbox duplicatePeer = listen(duplicate[1]); duplicatePeer.await(1);
        check(duplicatePeer.size() == 1, "duplicate rejection leaves owner usable");
        rejected(() -> closed[0].setWebMessageCallback(null, handler), "closed callback registration rejected");
        rejected(() -> closed[0].postMessage(new WebMessage("closed")), "closed post rejected");
        closed[0].close(); check(true, "close is idempotent");
        for (WebMessagePort port : new WebMessagePort[]{carrier[0],carrier[1],posted[0],posted[1],callback[0],callback[1],closed[1],fresh[1],accepted,duplicate[0],duplicate[1]}) port.close();
    }
    private static void closeBeforeDispatch() throws Exception {
        CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1), drained = new CountDownLatch(1);
        handler.post(() -> { entered.countDown(); try { release.await(4, TimeUnit.SECONDS); } catch (InterruptedException e) { Thread.currentThread().interrupt(); } });
        if (!entered.await(4, TimeUnit.SECONDS)) throw new AssertionError("Handler barrier missing");
        WebMessagePort[] ports = pair(); Inbox inbox = listen(ports[1]);
        try { ports[0].postMessage(new WebMessage("pending")); ports[1].close(); }
        finally { release.countDown(); }
        handler.post(drained::countDown);
        if (!drained.await(4, TimeUnit.SECONDS)) throw new AssertionError("Handler did not drain");
        check(inbox.size() == 0, "close discards queued messages before dispatch"); ports[0].close();
    }
    private interface Case { void run() throws Exception; }
    private static void runCase(String name, Case test) {
        try { test.run(); } catch (Throwable failure) {
            check(false, name + ": " + failure); failure.printStackTrace(System.err);
        }
    }
    public static void main(String[] args) throws Exception {
        if (args.length == 0 || !args[0].equals("--host")) System.loadLibrary("android_runtime");
        HandlerThread thread = new HandlerThread("webmessage-contracts"); thread.start();
        handler = new Handler(thread.getLooper());
        try {
            runCase("queue", WebMessagePortContractTest::queuedDelivery);
            runCase("transfer", WebMessagePortContractTest::localTransfer);
            runCase("both/retransfer", WebMessagePortContractTest::transferBothAndAgain);
            runCase("restrictions", WebMessagePortContractTest::transferRestrictions);
            runCase("close", WebMessagePortContractTest::closeBeforeDispatch);
        } finally {
            thread.quitSafely(); thread.join(5000);
            if (thread.isAlive()) throw new AssertionError("Handler thread did not terminate");
        }
        System.out.println("WebMessagePort contracts: " + checks + " checks, " + failures + " failures");
        if (failures != 0) throw new AssertionError("WebMessagePort contract failures");
    }
}
