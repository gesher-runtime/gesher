/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.webapp;

import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Picture;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.net.http.SslError;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Message;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.webkit.ClientCertRequest;
import android.webkit.GeolocationPermissions;
import android.webkit.HttpAuthHandler;
import android.webkit.JavascriptInterface;
import android.webkit.PermissionRequest;
import android.webkit.SslErrorHandler;
import android.webkit.ValueCallback;
import android.webkit.WebBackForwardList;
import android.webkit.WebChromeClient;
import android.webkit.WebMessage;
import android.webkit.WebMessagePort;
import android.webkit.WebResourceRequest;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.FrameLayout;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/**
 * The second WebView test activity (docs/webview.md, tests/windows/tracks/37-webview.ps1,
 * which reads what it says on stdout and clicks where it says). A WebView on
 * file:///android_asset/features.html (0,0 400x400, green) with a round blue button
 * (60x60 at 300,300) over it, and a WebView below (0,420 400x300) for pages of the test's
 * server ($GESHER_WEBAPP_SERVER: "httpPort httpsPort" on 127.0.0.1). In order:
 * WebMessagePorts, find in page, capturePicture, permissions, window.open/close, the file
 * choosers (the test clicks them), text zoom, then Basic authentication, postUrl, a
 * certificate error and a client certificate request, the back/forward list and a web
 * archive on the server's pages, then page-driven intent fallbacks with default,
 * accepting and target-vetoing clients; "Features done" at the end.
 */
public class FeaturesActivity extends Activity {
    private static final String TAG = "FeaturesActivity";
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private WebView mMain;
    private WebView mNav;
    private WebView mPopup;
    private WebMessagePort[] mChannel;
    private WebMessagePort mPagePort;
    private boolean mPortEcho, mPortQueued, mPortReply, mPortsFinished;
    private int mChoosers;
    private boolean mStarted;
    private String mServerHttp;
    private String mServerHttps;
    private Runnable mOnNavFinished;
    private volatile boolean mFabClicked;
    private volatile boolean mServerStarted;

    private static void say(String message) {
        System.out.println("Features " + message);
        System.out.flush();
        Log.i(TAG, message);
    }

    /** window.Features in features.html. */
    public final class Bridge {
        @JavascriptInterface
        public void report(String text) {
            say("page: " + text);
            if (text.startsWith("picked pickApp")) mHandler.post(FeaturesActivity.this::clickWin);
            if (text.startsWith("picked pickWin")) mHandler.postDelayed(FeaturesActivity.this::textZoom, 500);
            // After the button's click, the test clicks its corner: the page gets it.
            if (text.startsWith("page mousedown") && mFabClicked && !mServerStarted) {
                mServerStarted = true;
                mHandler.postDelayed(FeaturesActivity.this::serverSteps, 500);
            }
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        say("app location permission uid " + android.os.Process.myUid() + " pid "
                + android.os.Process.myPid() + " fine "
                + checkSelfPermission(android.Manifest.permission.ACCESS_FINE_LOCATION)
                + " coarse " + checkSelfPermission(android.Manifest.permission.ACCESS_COARSE_LOCATION));
        String server = System.getenv("GESHER_WEBAPP_SERVER");
        if (server != null && server.trim().contains(" ")) {
            String[] ports = server.trim().split("\\s+");
            mServerHttp = "http://127.0.0.1:" + ports[0];
            mServerHttps = "https://127.0.0.1:" + ports[1];
        }
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.WHITE);

        mMain = new WebView(this);
        mMain.getSettings().setJavaScriptEnabled(true);
        mMain.getSettings().setSupportMultipleWindows(true);
        mMain.getSettings().setJavaScriptCanOpenWindowsAutomatically(true);
        mMain.addJavascriptInterface(new Bridge(), "Features");
        mMain.setWebViewClient(new WebViewClient() {
            @Override
            public void onPageFinished(WebView view, String url) {
                say("onPageFinished: " + url);
                if (!mStarted && url.endsWith("features.html")) {
                    mStarted = true;
                    mHandler.postDelayed(FeaturesActivity.this::ports, 500);
                }
            }
        });
        mMain.setWebChromeClient(new WebChromeClient() {
            @Override
            public void onGeolocationPermissionsShowPrompt(String origin,
                    GeolocationPermissions.Callback callback) {
                say("onGeolocationPermissionsShowPrompt: " + origin);
                callback.invoke(origin, true, false);
            }

            @Override
            public void onPermissionRequest(PermissionRequest request) {
                String resource = request.getResources()[0];
                say("onPermissionRequest: " + request.getOrigin() + " " + resource);
                if (PermissionRequest.RESOURCE_VIDEO_CAPTURE.equals(resource)) {
                    request.grant(request.getResources());
                } else {
                    request.deny();
                }
            }

            @Override
            public boolean onCreateWindow(WebView view, boolean isDialog, boolean isUserGesture,
                    Message resultMsg) {
                say("onCreateWindow: dialog " + isDialog + ", user gesture " + isUserGesture);
                mPopup = new WebView(FeaturesActivity.this);
                mPopup.getSettings().setJavaScriptEnabled(true);
                mPopup.setWebViewClient(new WebViewClient() {
                    @Override
                    public void onPageFinished(WebView v, String url) {
                        say("popup onPageFinished: " + url);
                        v.evaluateJavascript("window.opener ? \"an opener\" : \"no opener\"",
                                o -> say("popup has " + o));
                        // The file choosers next, whether or not the popup closes itself.
                        mHandler.postDelayed(FeaturesActivity.this::clickApp, 3000);
                    }
                });
                mPopup.setWebChromeClient(new WebChromeClient() {
                    @Override
                    public void onCloseWindow(WebView window) {
                        say("onCloseWindow: " + (window == mPopup ? "the popup" : "another"));
                        mPopupClosed = true;
                        window.destroy();
                    }
                });
                ((WebView.WebViewTransport) resultMsg.obj).setWebView(mPopup);
                resultMsg.sendToTarget();
                return true;
            }

            @Override
            public boolean onShowFileChooser(WebView webView, ValueCallback<Uri[]> callback,
                    FileChooserParams params) {
                mChoosers++;
                say("onShowFileChooser " + mChoosers + ": mode " + params.getMode() + ", accept "
                        + String.join(" ", params.getAcceptTypes()));
                if (mChoosers > 1) return false;  // Gesher's Windows file dialog
                try {
                    File f = new File(getFilesDir(), "from-app.txt");
                    try (FileOutputStream out = new FileOutputStream(f)) {
                        out.write("hello from the app".getBytes(StandardCharsets.UTF_8));
                    }
                    callback.onReceiveValue(new Uri[] {Uri.fromFile(f)});
                } catch (Exception e) {
                    say("onShowFileChooser: " + e);
                    callback.onReceiveValue(null);
                }
                return true;
            }
        });
        root.addView(mMain, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, 400));

        // A round button over the WebView: only its circle is cut out of the WebView2 window.
        View fab = new View(this);
        GradientDrawable oval = new GradientDrawable();
        oval.setShape(GradientDrawable.OVAL);
        oval.setColor(Color.BLUE);
        fab.setBackground(oval);
        fab.setOnClickListener(v -> {
            say("fab clicked");
            mFabClicked = true;
            say("click corner");
        });
        FrameLayout.LayoutParams fabParams = new FrameLayout.LayoutParams(60, 60,
                Gravity.TOP | Gravity.START);
        fabParams.leftMargin = 300;
        fabParams.topMargin = 300;
        root.addView(fab, fabParams);

        mNav = new WebView(this);
        mNav.getSettings().setJavaScriptEnabled(true);
        mNav.setWebViewClient(new WebViewClient() {
            @Override
            public void onPageFinished(WebView view, String url) {
                say("nav onPageFinished: " + url);
                Runnable next = mOnNavFinished;
                mOnNavFinished = null;
                if (next != null) mHandler.postDelayed(next, 300);
            }

            @Override
            public void onReceivedHttpAuthRequest(WebView view, HttpAuthHandler handler,
                    String host, String realm) {
                say("onReceivedHttpAuthRequest: " + host + " realm " + realm);
                handler.proceed("gesher", "secret");
            }

            @Override
            public void onReceivedSslError(WebView view, SslErrorHandler handler, SslError error) {
                say("onReceivedSslError: " + error.getPrimaryError() + " for " + error.getUrl()
                        + ", issued to " + (error.getCertificate() != null
                        ? error.getCertificate().getIssuedTo().getCName() : "?"));
                handler.proceed();
            }

            @Override
            public void onReceivedClientCertRequest(WebView view, ClientCertRequest request) {
                say("onReceivedClientCertRequest: " + request.getHost() + ":" + request.getPort());
                request.ignore();
            }
        });
        FrameLayout.LayoutParams navParams = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, 300, Gravity.TOP);
        navParams.topMargin = 420;
        root.addView(mNav, navParams);
        setContentView(root);

        mMain.loadUrl("file:///android_asset/features.html");
    }

    // ---- The steps ----

    private void ports() {
        mChannel = mMain.createWebMessageChannel();
        mChannel[0].setWebMessageCallback(new WebMessagePort.WebMessageCallback() {
            @Override
            public void onMessage(WebMessagePort port, WebMessage message) {
                say("port message: " + message.getData());
                if ("page endpoint".equals(message.getData())) {
                    if (message.getPorts() == null || message.getPorts().length != 1) {
                        throw new AssertionError("Page-created MessagePort was not transferred");
                    }
                    mPagePort = message.getPorts()[0];
                    mPagePort.setWebMessageCallback(new WebMessagePort.WebMessageCallback() {
                        @Override public void onMessage(WebMessagePort received, WebMessage reply) {
                            if ("queued in page".equals(reply.getData()) && !mPortQueued) {
                                mPortQueued = true;
                                say("page port queued data preserved");
                            } else if ("reply from page".equals(reply.getData()) && mPortQueued) {
                                mPortReply = true;
                                received.close();
                                mPagePort = null;
                                say("page port reply and close passed");
                            } else {
                                throw new AssertionError("Unexpected page-port message: " + reply.getData());
                            }
                            finishPorts();
                        }
                    });
                    mPagePort.postMessage(new WebMessage("probe from Java"));
                } else if ("echo ping".equals(message.getData())) {
                    mPortEcho = true;
                    finishPorts();
                } else {
                    throw new AssertionError("Unexpected carrier message: " + message.getData());
                }
            }
        });
        mMain.postWebMessage(new WebMessage("hello page", new WebMessagePort[] {mChannel[1]}),
                Uri.parse("*"));
        mChannel[0].postMessage(new WebMessage("ping"));
    }

    private void finishPorts() {
        if (!mPortEcho || !mPortReply || mPortsFinished) return;
        mPortsFinished = true;
        mChannel[0].close();
        say("page-origin port round trip passed");
        mHandler.post(this::find);
    }

    private int mFindStep;

    private void find() {
        mMain.setFindListener((active, count, done) -> {
            if (!done) return;
            say("find: match " + active + " of " + count);
            if (mFindStep == 0 && count > 0) {
                mFindStep = 1;
                mHandler.postDelayed(() -> mMain.findNext(true), 300);
            } else if (mFindStep == 1 && active == 1) {
                mFindStep = 2;
                mMain.clearMatches();
                mHandler.postDelayed(this::picture, 300);
            }
        });
        mMain.findAllAsync("apple");
    }

    private void picture() {
        Picture p = mMain.capturePicture();
        Bitmap bmp = Bitmap.createBitmap(Math.max(1, p.getWidth()), Math.max(1, p.getHeight()),
                Bitmap.Config.ARGB_8888);
        new Canvas(bmp).drawPicture(p);
        say(String.format("capturePicture: %dx%d, at 200,300 #%06x", p.getWidth(), p.getHeight(),
                bmp.getPixel(Math.min(200, bmp.getWidth() - 1), Math.min(300, bmp.getHeight() - 1))
                        & 0xffffff));
        mHandler.postDelayed(this::permissions, 300);
    }

    private void permissions() {
        mMain.evaluateJavascript("askGeo(); askMedia('camera');"
                + " setTimeout(function () { askMedia('microphone'); }, 1500)", null);
        mHandler.postDelayed(this::popup, 7000);
    }

    private void popup() {
        mMain.evaluateJavascript("openPopup()", null);
    }

    private boolean mClickedApp;
    private boolean mPopupClosed;

    private void clickApp() {
        if (mClickedApp) return;
        mClickedApp = true;
        if (mPopup != null && !mPopupClosed) {
            // It did not close itself (runtime 131): the app closes it, as one would.
            say("popup still open: destroying it");
            mPopup.destroy();
        }
        mMain.evaluateJavascript("place('pickApp')", value -> say("click pickApp at "
                + inWindow(value)));
    }

    private void clickWin() {
        mMain.evaluateJavascript("place('pickWin')", value -> say("click pickWin at "
                + inWindow(value)));
    }

    /** "x,y" in the page (CSS pixels, a JSON string) as window coordinates. */
    private String inWindow(String json) {
        String[] xy = json.replace("\"", "").split(",");
        int[] at = new int[2];
        mMain.getLocationInWindow(at);
        float d = getResources().getDisplayMetrics().density;
        return Math.round(at[0] + Integer.parseInt(xy[0].trim()) * d) + ","
                + Math.round(at[1] + Integer.parseInt(xy[1].trim()) * d);
    }

    private void textZoom() {
        mMain.getSettings().setTextZoom(200);
        mHandler.postDelayed(() -> mMain.evaluateJavascript("zoomed()", v -> {
            say("text zoom 200: " + v);
            mMain.getSettings().setTextZoom(100);
            mHandler.postDelayed(() -> mMain.evaluateJavascript("zoomed()", v2 -> {
                say("text zoom 100: " + v2);
                say("click fab");
            }), 800);
        }), 800);
    }

    /** The test clicked the button and the page around it: the server's pages next. */
    private void serverSteps() {
        if (mServerHttp == null) {
            say("no server ($GESHER_WEBAPP_SERVER)");
            say("done");
            return;
        }
        mOnNavFinished = () -> title("auth", () -> {
            mOnNavFinished = () -> title("postUrl", () -> {
                mOnNavFinished = () -> title("tls", this::history);
                mNav.loadUrl(mServerHttps + "/");
            });
            mNav.postUrl(mServerHttp + "/post", "a=1&b=two".getBytes(StandardCharsets.UTF_8));
        });
        mNav.loadUrl(mServerHttp + "/auth");
    }

    private void title(String what, Runnable next) {
        mNav.evaluateJavascript("document.title", v -> {
            say(what + " title: " + v);
            next.run();
        });
    }

    private void history() {
        mHandler.postDelayed(() -> {
            WebBackForwardList list = mNav.copyBackForwardList();
            StringBuilder s = new StringBuilder();
            for (int i = 0; i < list.getSize(); i++) {
                if (i > 0) s.append(" | ");
                s.append(list.getItemAtIndex(i).getUrl());
            }
            say("history: " + list.getSize() + " item(s), current " + list.getCurrentIndex() + ": "
                    + s);
            say("canGoBackOrForward(-2): " + mNav.canGoBackOrForward(-2));
            mOnNavFinished = this::archive;
            mNav.goBackOrForward(-2);
        }, 500);
    }

    private void archive() {
        File file = new File(getCacheDir(), "page.mht");
        mNav.saveWebArchive(file.getPath(), false, path -> {
            String head = "";
            try {
                byte[] data = Files.readAllBytes(new File(path).toPath());
                String text = new String(data, StandardCharsets.UTF_8);
                head = data.length + " bytes, multipart/related " + text.contains("multipart/related")
                        + ", the page's text " + text.contains("auth ok");
            } catch (Exception e) {
                head = String.valueOf(e);
            }
            say("saveWebArchive: " + (path != null ? new File(path).getName() : "null") + ", "
                    + head);
            intentStep(0);
        });
    }

    private void intentStep(int step) {
        if (step == 3) { downloadStep(0); return; }
        String name = new String[] {"default", "explicit", "veto"}[step];
        IntentObserver observer = step == 0 ? new IntentObserver(name, step)
                : new IntentObserver(name, step) {
                    @Override public boolean shouldOverrideUrlLoading(WebView view,
                            WebResourceRequest request) {
                        return overrideIntent(request, step == 2);
                    }
                };
        mNav.setWebViewClient(observer);
        mHandler.postDelayed(observer.timeout, 30000);
        mNav.loadUrl(observer.source);
    }

    /** Real attachment/unsupported-MIME handoff leaves the loaded page usable. */
    private void downloadStep(int step) {
        if (step == 4) { authenticatedDownloadStep(0); return; }
        String name = new String[] {"attachment", "mime", "redirect", "auth"}[step];
        final boolean[] clicked = {false};
        final int[] downloads = {0}, errors = {0};
        String source = mServerHttp + "/download-source/" + name;
        mNav.setDownloadListener((url, agent, disposition, mime, length) -> {
            if (!url.equals(mServerHttp + "/download-file/" + (step == 2 ? "attachment" : name))
                    || length <= 0 || !mime.equals("application/octet-stream")) {
                throw new AssertionError("Wrong download metadata: " + url + " " + mime + " " + length);
            }
            downloads[0]++;
        });
        mNav.setWebViewClient(new WebViewClient() {
            @Override public void onReceivedHttpAuthRequest(WebView view, HttpAuthHandler handler,
                    String host, String realm) {
                if (!name.equals("auth") || !realm.equals("gesher-download")) {
                    throw new AssertionError("Unexpected download authentication: " + realm);
                }
                say("download auth credentials requested");
                handler.proceed("download", "secret");
            }
            @Override public void onReceivedError(WebView view, WebResourceRequest request,
                    android.webkit.WebResourceError error) {
                if (request.isForMainFrame()) errors[0]++;
            }
            @Override public void onPageFinished(WebView view, String url) {
                if (!url.equals(source) || clicked[0]) return;
                clicked[0] = true;
                mNav.evaluateJavascript("document.getElementById('download').click()", null);
                mHandler.postDelayed(() -> {
                    if (downloads[0] != 1 || errors[0] != 0) {
                        throw new AssertionError("Download " + name + " callbacks " + downloads[0]
                                + " errors " + errors[0]);
                    }
                    mNav.evaluateJavascript("document.title", title -> {
                        if (!title.equals("\"download source " + name + "\"")) {
                            throw new AssertionError("Download replaced source: " + title);
                        }
                        say("download " + name + " handed off once without network error; source retained");
                        downloadStep(step + 1);
                    });
                }, 2000);
            }
        });
        mNav.loadUrl(source);
    }

    /** Challenge/retry must authenticate the original request without replacing its source. */
    private void authenticatedDownloadStep(int step) {
        if (step == 6) { downloadIsolation(0); return; }
        String name = new String[] {"retry", "cancel", "post", "largepost", "redirect", "stop"}[step];
        String source = mServerHttp + "/auth-download-source/" + name;
        String target = mServerHttp + "/auth-download/" + name;
        int[] challenges = {0}, downloads = {0}, errors = {0}, httpErrors = {0};
        boolean[] started = {false}, complete = {false}, verificationQueued = {false};
        Runnable timeout = () -> { if (!complete[0]) throw new AssertionError("Auth " + name + " timed out"); };
        mHandler.postDelayed(timeout, 15000);
        Runnable verify = () -> {
            int expectedDownloads = (name.equals("cancel") || name.equals("stop")) ? 0 : 1;
            int expectedChallenges = name.equals("retry") ? 2 : 1;
            if (downloads[0] != expectedDownloads || challenges[0] != expectedChallenges || errors[0] != 0
                    || httpErrors[0] != (name.equals("cancel") ? 1 : 0)) {
                throw new AssertionError("Auth " + name + " callbacks: challenges=" + challenges[0]
                        + ", downloads=" + downloads[0] + ", errors=" + errors[0] + ", HTTP=" + httpErrors[0]);
            }
            String script = name.equals("cancel") ? "document.title"
                    : "document.title+'|'+window.authFixtureMarker+'|'+location.href";
            mNav.evaluateJavascript(script, value -> {
                String expected = name.equals("cancel") ? "auth denied cancel" : "auth source " + name + "|38|" + source;
                if (!value.equals("\"" + expected + "\"")) {
                    throw new AssertionError("Auth " + name + " document changed: " + value);
                }
                complete[0] = true;
                mHandler.removeCallbacks(timeout);
                say("download auth " + name + " original request and consent preserved");
                authenticatedDownloadStep(step + 1);
            });
        };
        mNav.setDownloadListener((url, agent, disposition, mime, length) -> {
            if (!url.equals(target) || !mime.equals("application/octet-stream") || length <= 0
                    || ++downloads[0] != 1 || name.equals("cancel") || name.equals("stop")) {
                throw new AssertionError("Unexpected auth " + name + " download " + url);
            }
            if (verificationQueued[0]) throw new AssertionError("Duplicate auth terminal " + name);
            verificationQueued[0] = true;
            mHandler.postDelayed(verify, 500);
        });
        mNav.setWebViewClient(new WebViewClient() {
            @Override public void onReceivedHttpAuthRequest(WebView view, HttpAuthHandler handler,
                    String host, String realm) {
                int attempt = ++challenges[0];
                if (!realm.equals("gesher-auth-" + name) || attempt > (name.equals("retry") ? 2 : 1)) {
                    throw new AssertionError("Wrong/repeated auth challenge " + realm + " " + attempt);
                }
                if (name.equals("cancel")) handler.cancel();
                else if (name.equals("stop")) {
                    view.stopLoading();
                    mHandler.postDelayed(() -> handler.proceed("download", "secret-" + name), 150);
                    verificationQueued[0] = true;
                    mHandler.postDelayed(verify, 700);
                } else handler.proceed("download", name.equals("retry") && attempt == 1 ? "wrong" : "secret-" + name);
            }
            @Override public void onReceivedError(WebView view, WebResourceRequest request,
                    android.webkit.WebResourceError error) {
                if (request.isForMainFrame()) errors[0]++;
            }
            @Override public void onReceivedHttpError(WebView view, WebResourceRequest request,
                    android.webkit.WebResourceResponse response) {
                if (request.isForMainFrame()) {
                    if (!name.equals("cancel") || response.getStatusCode() != 401) {
                        throw new AssertionError("Auth " + name + " exposed an intermediate denial");
                    }
                    httpErrors[0]++;
                }
            }
            @Override public void onPageFinished(WebView view, String url) {
                if (url.equals(source) && !started[0]) {
                    started[0] = true;
                    mNav.evaluateJavascript("window.authFixtureMarker=38;"
                            + (name.equals("largepost") ? "document.getElementById('auth-payload').value='x'.repeat(2<<20);" : "")
                            + ((name.equals("post") || name.equals("largepost")) ? "document.getElementById('auth-form').submit()"
                            : "document.getElementById('auth-download').click()"), null);
                } else if (name.equals("cancel") && url.equals(target) && !verificationQueued[0]) {
                    verificationQueued[0] = true;
                    mHandler.postDelayed(verify, 500);
                }
            }
        });
        mNav.loadUrl(source);
    }

    /** A past download, including one in a child frame, must not mask a new request. */
    private void downloadIsolation(int step) {
        if (step == 2) { downloadFailure(); return; }
        String name = step == 0 ? "repeat-main" : "repeat-frame";
        String source = mServerHttp + "/download-source/" + name;
        String target = mServerHttp + "/download-repeat/" + name;
        final int[] downloads = {0}, errors = {0};
        final boolean[] clicked = {false}, failing = {false}, complete = {false};
        Runnable timeout = () -> { if (!complete[0]) throw new AssertionError(name + " timed out"); };
        mHandler.postDelayed(timeout, 15000);
        mNav.setDownloadListener((url, agent, disposition, mime, length) -> {
            if (!url.equals(target) || !mime.equals("application/octet-stream") || length <= 0
                    || ++downloads[0] != 1 || failing[0]) {
                throw new AssertionError("Unexpected " + name + " download " + url);
            }
            mHandler.postDelayed(() -> {
                if (errors[0] != 0) throw new AssertionError(name + " handoff reported a network error");
                mNav.evaluateJavascript("document.title", title -> {
                    if (!title.equals("\"download source " + name + "\"")) {
                        throw new AssertionError(name + " replaced its source document: " + title);
                    }
                    failing[0] = true;
                    mNav.loadUrl(target); // The server now closes this exact URL without a response.
                });
            }, 500);
        });
        mNav.setWebViewClient(new WebViewClient() {
            @Override public void onReceivedError(WebView view, WebResourceRequest request,
                    android.webkit.WebResourceError failure) {
                if (request.isForMainFrame()) {
                    if (!failing[0] || !request.getUrl().toString().equals(target)) {
                        throw new AssertionError("Unexpected " + name + " main-frame error");
                    }
                    errors[0]++;
                }
            }
            @Override public void onPageFinished(WebView view, String url) {
                if (url.equals(source) && !clicked[0]) {
                    clicked[0] = true;
                    mNav.evaluateJavascript("document.getElementById('download').click()", null);
                } else if (url.equals(target) && failing[0] && !complete[0]) {
                    if (errors[0] != 1 || downloads[0] != 1) {
                        throw new AssertionError(name + " hid or duplicated the repeated request failure: "
                                + errors[0] + " errors, " + downloads[0] + " downloads");
                    }
                    complete[0] = true;
                    mHandler.removeCallbacks(timeout);
                    say("download " + name + " same URL network failure preserved");
                    mHandler.post(() -> downloadIsolation(step + 1));
                }
            }
        });
        mNav.loadUrl(source);
    }

    private void downloadFailure() {
        mNav.setDownloadListener((url, agent, disposition, mime, length) -> {
            throw new AssertionError("Network failure became download");
        });
        final boolean[] error = {false};
        mNav.setWebViewClient(new WebViewClient() {
            @Override public void onReceivedError(WebView view, WebResourceRequest request,
                    android.webkit.WebResourceError failure) {
                if (request.isForMainFrame()) error[0] = true;
            }
            @Override public void onPageFinished(WebView view, String url) {
                if (!error[0]) throw new AssertionError("Real network failure hidden");
                say("download adjacent network failure preserved");
                mHandler.post(() -> truncatedDownloadResponse(0));
            }
        });
        mNav.loadUrl(mServerHttp + "/network-failure");
    }

    /** COM completion can precede the CDP body failure: preserve one ordered terminal. */
    private void truncatedDownloadResponse(int step) {
        boolean httpFailure = step == 1;
        String name = httpFailure ? "http500-truncated" : "http200-truncated";
        String target = mServerHttp + "/" + name;
        int[] errors = {0}, httpErrors = {0}, finishes = {0};
        boolean[] complete = {false};
        Runnable timeout = () -> { if (!complete[0]) throw new AssertionError(name + " timed out"); };
        mHandler.postDelayed(timeout, 10000);
        mNav.setWebViewClient(new WebViewClient() {
            @Override public void onReceivedError(WebView view, WebResourceRequest request,
                    android.webkit.WebResourceError failure) {
                if (request.isForMainFrame()) {
                    if (finishes[0] != 0) throw new AssertionError(name + " network error arrived after finish");
                    errors[0]++;
                }
            }
            @Override public void onReceivedHttpError(WebView view, WebResourceRequest request,
                    android.webkit.WebResourceResponse response) {
                if (request.isForMainFrame()) {
                    if (response.getStatusCode() != 500 || finishes[0] != 0) {
                        throw new AssertionError(name + " HTTP error order/status");
                    }
                    httpErrors[0]++;
                }
            }
            @Override public void onPageFinished(WebView view, String url) {
                if (!url.equals(target)) throw new AssertionError(name + " wrong terminal URL " + url);
                if (++finishes[0] != 1 || errors[0] != (httpFailure ? 0 : 1)
                        || httpErrors[0] != (httpFailure ? 1 : 0)) {
                    throw new AssertionError(name + " terminal order/count: network=" + errors[0]
                            + ", HTTP=" + httpErrors[0] + ", finish=" + finishes[0]);
                }
                mHandler.postDelayed(() -> {
                    if (finishes[0] != 1 || errors[0] != (httpFailure ? 0 : 1)) {
                        throw new AssertionError(name + " duplicated late terminal");
                    }
                    complete[0] = true;
                    mHandler.removeCallbacks(timeout);
                    say("download " + name + " one ordered error and finish preserved");
                    if (httpFailure) authenticatedTwoViews(); else truncatedDownloadResponse(1);
                }, 500);
            }
        });
        mNav.loadUrl(target);
    }

    /** Identical URLs in different views must route to independent app consent handlers. */
    private void authenticatedTwoViews() {
        WebView[] pages = {mNav, new WebView(this)};
        FrameLayout parent = (FrameLayout) mNav.getParent();
        pages[1].getSettings().setJavaScriptEnabled(true);
        parent.addView(pages[1], new FrameLayout.LayoutParams(1, 1));
        String source = mServerHttp + "/auth-download-source/twoviews";
        String target = mServerHttp + "/auth-download/twoviews";
        HttpAuthHandler[] pending = {null, null};
        int[] challenges = {0, 0}, downloads = {0, 0}, errors = {0, 0};
        boolean[] loaded = {false, false}, canceledFinished = {false}, done = {false}, queued = {false};
        Runnable timeout = () -> { if (!done[0]) throw new AssertionError("Independent auth views timed out"); };
        mHandler.postDelayed(timeout, 15000);
        Runnable verify = () -> {
            if (downloads[0] != 1 || !canceledFinished[0] || queued[0]) return;
            queued[0] = true;
            mHandler.postDelayed(() -> {
                if (challenges[0] != 1 || challenges[1] != 1 || downloads[1] != 0
                        || errors[0] != 0 || errors[1] != 0) {
                    throw new AssertionError("Same-URL views shared authentication consent");
                }
                pages[0].evaluateJavascript("document.title+'|'+window.authFixtureMarker+'|'+location.href", title -> {
                    if (!title.equals("\"auth source twoviews|100|" + source + "\"")) {
                        throw new AssertionError("Independent allowed view lost its live source " + title);
                    }
                    pages[1].evaluateJavascript("document.title", denied -> {
                        if (!denied.equals("\"auth denied twoviews\"")) {
                            throw new AssertionError("Independent denied view used the other view's credentials " + denied);
                        }
                        done[0] = true;
                        mHandler.removeCallbacks(timeout);
                        pages[1].destroy();
                        parent.removeView(pages[1]);
                        say("download auth same URL views kept independent consent");
                        authenticatedDownloadDestroy();
                    });
                });
            }, 500);
        };
        for (int i = 0; i < pages.length; i++) {
            final int owner = i;
            pages[i].setDownloadListener((url, agent, disposition, mime, length) -> {
                if (owner != 0 || !url.equals(target) || ++downloads[owner] != 1) {
                    throw new AssertionError("Wrong auth owner received a download " + owner);
                }
                verify.run();
            });
            pages[i].setWebViewClient(new WebViewClient() {
                @Override public void onReceivedHttpAuthRequest(WebView view, HttpAuthHandler handler,
                        String host, String realm) {
                    if (view != pages[owner] || ++challenges[owner] != 1
                            || !realm.equals("gesher-auth-twoviews")) {
                        throw new AssertionError("Auth challenge crossed its view identity");
                    }
                    pending[owner] = handler;
                    if (pending[0] != null && pending[1] != null) {
                        pending[1].cancel();
                        mHandler.postDelayed(() -> pending[0].proceed("download", "secret-twoviews"), 150);
                    }
                }
                @Override public void onReceivedError(WebView view, WebResourceRequest request,
                        android.webkit.WebResourceError error) {
                    if (request.isForMainFrame()) errors[owner]++;
                }
                @Override public void onReceivedHttpError(WebView view, WebResourceRequest request,
                        android.webkit.WebResourceResponse response) {
                    if (request.isForMainFrame() && (owner != 1 || response.getStatusCode() != 401)) {
                        throw new AssertionError("Wrong independent-view HTTP denial");
                    }
                }
                @Override public void onPageFinished(WebView view, String url) {
                    if (url.equals(source) && !loaded[owner]) {
                        loaded[owner] = true;
                        if (loaded[0] && loaded[1]) {
                            for (int n = 0; n < pages.length; n++) {
                                pages[n].evaluateJavascript("window.authFixtureMarker=" + (100 + n)
                                        + ";document.getElementById('auth-download').click()", null);
                            }
                        }
                    } else if (owner == 1 && url.equals(target)) {
                        canceledFinished[0] = true;
                        verify.run();
                    }
                }
            });
            pages[i].loadUrl(source);
        }
    }

    /** A retired view cannot resume a request when an old auth handler is answered. */
    private void authenticatedDownloadDestroy() {
        String source = mServerHttp + "/auth-download-source/destroy";
        boolean[] started = {false}, destroyed = {false}, complete = {false};
        Runnable timeout = () -> { if (!complete[0]) throw new AssertionError("Auth destroy timed out"); };
        mHandler.postDelayed(timeout, 15000);
        mNav.setDownloadListener((url, agent, disposition, mime, length) -> {
            throw new AssertionError("Destroyed authenticated request produced a download");
        });
        mNav.setWebViewClient(new WebViewClient() {
            @Override public void onReceivedHttpAuthRequest(WebView view, HttpAuthHandler handler,
                    String host, String realm) {
                if (destroyed[0] || !realm.equals("gesher-auth-destroy")) {
                    throw new AssertionError("Wrong/repeated destroy challenge " + realm);
                }
                destroyed[0] = true;
                view.destroy();
                mHandler.postDelayed(() -> handler.proceed("download", "secret-destroy"), 150);
                mHandler.postDelayed(() -> {
                    complete[0] = true;
                    mHandler.removeCallbacks(timeout);
                    say("download auth destroy ignored late credentials");
                    say("done");
                }, 700);
            }
            @Override public void onReceivedError(WebView view, WebResourceRequest request,
                    android.webkit.WebResourceError error) {
                throw new AssertionError("Destroyed auth request produced an error callback");
            }
            @Override public void onPageFinished(WebView view, String url) {
                if (destroyed[0]) throw new AssertionError("Destroyed auth request produced page finish");
                if (url.equals(source) && !started[0]) {
                    started[0] = true;
                    mNav.evaluateJavascript("document.getElementById('auth-download').click()", null);
                }
            }
        });
        mNav.loadUrl(source);
    }

    /** The default case inherits shouldOverrideUrlLoading, but observes real page callbacks. */
    private class IntentObserver extends WebViewClient {
        final String name;
        final int step;
        final String sourcePath, targetPath, source, target;
        final Runnable timeout;
        int overrides, targetStarts, targetFinishes;
        boolean sourceSeen, complete;

        IntentObserver(String name, int step) {
            this.name = name;
            this.step = step;
            sourcePath = "/intent-source/" + name;
            targetPath = "/intent-target/" + name;
            source = mServerHttp + sourcePath;
            target = mServerHttp + targetPath;
            timeout = () -> { if (!complete) throw new AssertionError("Intent " + name + " timed out"); };
        }

        boolean overrideIntent(WebResourceRequest request, boolean veto) {
            String url = request.getUrl().toString();
            if (url.startsWith("intent:")) {
                if (overrides++ != 0 || !request.isForMainFrame()) {
                    throw new AssertionError("Intent original callback order: " + name);
                }
                say("intent " + name + " callback 1 original allowed");
                return false;
            }
            if (url.equals(target)) {
                if (overrides++ != 1 || !request.isForMainFrame()) {
                    throw new AssertionError("Intent target callback order: " + name);
                }
                say("intent " + name + " callback 2 target veto " + veto);
                if (veto) mHandler.postDelayed(this::verifyVeto, 500);
                return veto;
            }
            return false;
        }

        @Override public void onPageStarted(WebView view, String url, Bitmap icon) {
            if (url.equals(target)) {
                targetStarts++;
                if (step == 2) throw new AssertionError("Vetoed intent target started");
                say("intent " + name + " target onPageStarted");
            }
        }

        @Override public void onPageFinished(WebView view, String url) {
            if (url.equals(source) && !sourceSeen) {
                sourceSeen = true;
                verifyDocument("source", sourcePath, () -> {
                    say("intent " + name + " source onPageFinished");
                    // The server page clicks its own intent: anchor. This is not
                    // an application loadUrl(intent), which skips the callback.
                    view.evaluateJavascript("runIntent()", null);
                });
            } else if (url.equals(target)) {
                targetFinishes++;
                if (step == 2) throw new AssertionError("Vetoed intent target finished");
                verifyDocument("target", targetPath, () -> {
                    if (!sourceSeen || targetStarts != 1 || targetFinishes != 1
                            || overrides != (step == 0 ? 0 : 2)) {
                        throw new AssertionError("Intent destination callback counts: " + name);
                    }
                    say("intent " + name + " destination verified callbacks " + overrides
                            + " started " + targetStarts + " finished " + targetFinishes);
                    finishIntent();
                });
            }
        }

        private void verifyDocument(String kind, String path, Runnable next) {
            mNav.evaluateJavascript("document.title + '|' + location.pathname", value -> {
                String expected = "\"intent " + kind + " " + name + "|" + path + "\"";
                if (!expected.equals(value)) {
                    throw new AssertionError("Intent " + name + " document: " + value);
                }
                next.run();
            });
        }

        private void verifyVeto() {
            verifyDocument("source", sourcePath, () -> {
                if (!sourceSeen || overrides != 2 || targetStarts != 0 || targetFinishes != 0) {
                    throw new AssertionError("Intent target veto was not retained");
                }
                say("intent veto retained source callbacks 2 started 0 finished 0");
                finishIntent();
            });
        }

        private void finishIntent() {
            if (complete) throw new AssertionError("Intent " + name + " completed twice");
            complete = true;
            mHandler.removeCallbacks(timeout);
            mHandler.post(() -> intentStep(step + 1));
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        say("onResume");
    }

    @Override
    protected void onDestroy() {
        say("onDestroy");
        if (mPagePort != null) mPagePort.close();
        if (mChannel != null) mChannel[0].close();
        super.onDestroy();
    }
}
