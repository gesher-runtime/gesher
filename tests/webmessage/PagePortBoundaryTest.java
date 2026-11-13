package com.gesher.app.webview;

import android.webkit.WebMessage;
import android.webkit.WebMessagePort;
import org.json.JSONObject;

/** Exact provider transfer/post methods; WebView2 transport is an observed boundary. */
public final class PagePortBoundaryTest {
    private static int checks, failures;
    private static void check(boolean ok, String label) {
        checks++; if (!ok) { failures++; System.err.println("FAIL: " + label); }
    }
    private static void rejected(Runnable call, String label) {
        try { call.run(); check(false, label); }
        catch (IllegalStateException expected) { check(true, label); }
    }
    private static void queuedBeforePage() throws Exception {
        WebMessagePort[] pair = GesherWebMessagePort.pair();
        pair[0].postMessage(new WebMessage("queued"));
        GesherWebViewProvider page = new GesherWebViewProvider();
        page.postMessageToMainFrame(new WebMessage("initial", new WebMessagePort[]{pair[1]}), null);
        check(page.mHost.sent.size() == 2, "queued message forwarded after page port creation");
        JSONObject initial = new JSONObject(page.mHost.sent.get(0));
        check("post".equals(initial.getString("gesher")), "page receives creation first");
        if (page.mHost.sent.size() > 1) {
            JSONObject queued = new JSONObject(page.mHost.sent.get(1));
            check("port".equals(queued.getString("gesher")), "queued endpoint event follows creation");
            check("queued".equals(queued.getString("data")), "queued payload retained");
            check(initial.getJSONArray("ports").getString(0).equals(queued.getString("id")), "queued event addresses created endpoint");
        }
        rejected(() -> pair[1].setWebMessageCallback(null), "page transfer detaches callback API");
        rejected(pair[1]::close, "page transfer detaches close API");
        pair[0].close();
        check(page.mPagePorts.isEmpty(), "app close releases page registry");
    }
    private static void validation() throws Exception {
        WebMessagePort[] good = GesherWebMessagePort.pair(), used = GesherWebMessagePort.pair();
        used[0].postMessage(new WebMessage("started"));
        GesherWebViewProvider page = new GesherWebViewProvider();
        rejected(() -> page.postMessageToMainFrame(new WebMessage("invalid", new WebMessagePort[]{good[0],used[0]}), null), "page rejects started transfer");
        check(page.mPagePorts.isEmpty(), "invalid batch leaves page registry unchanged");
        check(page.mHost.sent.isEmpty(), "invalid batch sends no page event");
        page.postMessageToMainFrame(new WebMessage("valid", new WebMessagePort[]{good[0]}), null);
        check(page.mPagePorts.size() == 1, "earlier valid endpoint remains owned after rejected batch");
        good[1].close(); used[0].close(); used[1].close();
    }
    private static void bothOnPage() throws Exception {
        WebMessagePort[] pair = GesherWebMessagePort.pair();
        GesherWebViewProvider page = new GesherWebViewProvider();
        page.postMessageToMainFrame(new WebMessage("pair", pair), null);
        ((GesherWebMessagePort) pair[0]).fromPage(new WebMessage("peer"));
        check(page.mHost.sent.size() == 2, "both page endpoints relay through their peer");
        if (page.mHost.sent.size() > 1) {
            JSONObject event = new JSONObject(page.mHost.sent.get(1));
            check(event.getString("id").equals(((GesherWebMessagePort)pair[1]).mId), "relay addresses other endpoint");
            check("peer".equals(event.getString("data")), "relay payload");
        }
        ((GesherWebMessagePort)pair[1]).pageGone();
        ((GesherWebMessagePort)pair[0]).fromPage(new WebMessage("discard"));
        check(page.mHost.sent.size() <= 2, "navigation discards messages for gone page");
    }
    private static void nestedQueuedTransfer() throws Exception {
        WebMessagePort[] carrier = GesherWebMessagePort.pair(), nested = GesherWebMessagePort.pair();
        carrier[0].postMessage(new WebMessagePayload(new byte[]{1,2,3},new WebMessagePort[]{nested[0]}));
        GesherWebViewProvider page = new GesherWebViewProvider();
        page.postMessageToMainFrame(new WebMessage("initial",new WebMessagePort[]{carrier[1]}),null);
        check(page.mPagePorts.size() == 2, "queued transferred endpoint reaches page ownership");
        check(page.mHost.sent.size() == 2, "queued typed transfer sends creation and payload");
        if (page.mHost.sent.size() > 1) {
            JSONObject event = new JSONObject(page.mHost.sent.get(1));
            check(event.getBoolean("binary"), "binary type preserved across local transfer");
            check("AQID".equals(event.getString("data")), "binary bytes preserved across local transfer");
            check(event.getJSONArray("ports").length() == 1, "nested port reaches page payload");
        }
        carrier[0].close(); nested[1].close();
    }
    private interface Case { void run() throws Exception; }
    private static void runCase(String name, Case test) {
        try { test.run(); } catch (Throwable error) { check(false,name + ": " + error); error.printStackTrace(System.err); }
    }
    public static void main(String[] args) {
        runCase("queue",PagePortBoundaryTest::queuedBeforePage);
        runCase("validation",PagePortBoundaryTest::validation);
        runCase("both",PagePortBoundaryTest::bothOnPage);
        runCase("nested",PagePortBoundaryTest::nestedQueuedTransfer);
        System.out.println("WebMessage page boundary: " + checks + " checks, " + failures + " failures");
        if (failures != 0) throw new AssertionError("Page boundary failures");
    }
}
