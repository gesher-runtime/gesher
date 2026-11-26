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
        if (step == 3) { say("done"); return; }
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
