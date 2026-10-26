package com.gesher.app.webview;

import android.content.Context;
import android.net.Uri;
import android.os.Handler;
import android.util.Log;
import android.webkit.ValueCallback;
import android.webkit.WebMessage;
import android.webkit.WebMessagePort;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;
import java.util.Set;
import java.util.concurrent.Callable;
import java.util.concurrent.Executor;

/**
 * androidx.webkit's glue (ADR 0058): what {@code WebViewCompat}, {@code WebSettingsCompat},
 * {@code ProxyController} and the other androidx.webkit APIs reach through Chromium's
 * "support library boundary". androidx.webkit loads
 * {@code org.chromium.support_lib_glue.SupportLibReflectionUtil} from the WebView provider's
 * class loader (gesher-app's) and calls its {@code createWebViewProviderFactory()}; every
 * object crosses the boundary as an {@link InvocationHandler}, the other side's interfaces
 * being proxies over it.
 *
 * <p>Gesher's side: {@link Boundary} answers a boundary interface's method with the
 * delegate's public method of the same name and parameter types (all of them framework or
 * java types: the boundary passes its own objects as InvocationHandlers), and calls the
 * app's handlers with a Method of the app's copy of the boundary interface. The features
 * Gesher implements are those {@link Factory#getSupportedFeatures} lists; androidx.webkit
 * checks them ({@code WebViewFeature.isFeatureSupported}) before it calls anything.
 */
public final class SupportLib {
    private static final String TAG = GesherWebView.TAG;
    private static final String BOUNDARY = "org.chromium.support_lib_boundary.";

    private SupportLib() {
    }

    /**
     * Chromium's feature names (org.chromium.support_lib_boundary.util.Features) that
     * Gesher's WebView implements. The framework-backed ones (WebView APIs since M..Q) are
     * listed too, as Chromium lists them.
     */
    static final String[] FEATURES = {
        "VISUAL_STATE_CALLBACK", "OFF_SCREEN_PRERASTER", "SAFE_BROWSING_ENABLE",
        "DISABLED_ACTION_MODE_MENU_ITEMS", "START_SAFE_BROWSING", "SAFE_BROWSING_ALLOWLIST",
        "SAFE_BROWSING_WHITELIST", "SAFE_BROWSING_PRIVACY_POLICY_URL",
        "RECEIVE_WEB_RESOURCE_ERROR", "RECEIVE_HTTP_ERROR", "SHOULD_OVERRIDE_WITH_REDIRECTS",
        "WEB_RESOURCE_REQUEST_IS_REDIRECT", "WEB_RESOURCE_ERROR_GET_DESCRIPTION",
        "WEB_RESOURCE_ERROR_GET_CODE", "WEB_MESSAGE_PORT_POST_MESSAGE", "WEB_MESSAGE_PORT_CLOSE",
        "WEB_MESSAGE_PORT_SET_MESSAGE_CALLBACK", "CREATE_WEB_MESSAGE_CHANNEL",
        "POST_WEB_MESSAGE", "WEB_MESSAGE_CALLBACK_ON_MESSAGE", "GET_WEB_VIEW_CLIENT",
        "GET_WEB_CHROME_CLIENT", "WEB_MESSAGE_LISTENER", "DOCUMENT_START_SCRIPT",
        "DOCUMENT_START_SCRIPT:1", "PROXY_OVERRIDE", "PROXY_OVERRIDE:3",
        "MULTI_PROCESS_QUERY", "GET_VARIATIONS_HEADER", "WEB_MESSAGE_ARRAY_BUFFER",
    };

    /** What androidx.webkit's WebViewGlueCommunicator gets (SupportLibReflectionUtil). */
    public static InvocationHandler createWebViewProviderFactory() {
        GesherWebView.log("androidx.webkit: the support library glue is Gesher's ("
                + FEATURES.length + " features)");
        return Boundary.of(new Factory());
    }

    // ---- The boundary ----

    /** An InvocationHandler over a Gesher delegate (BoundaryInterfaceReflectionUtil's). */
    static final class Boundary implements InvocationHandler {
        final Object delegate;

        private Boundary(Object delegate) {
            this.delegate = delegate;
        }

        static InvocationHandler of(Object delegate) {
            return delegate != null ? new Boundary(delegate) : null;
        }

        @Override
        public Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
            Method target;
            try {
                target = delegate.getClass().getMethod(method.getName(), method.getParameterTypes());
            } catch (NoSuchMethodException e) {
                if ("getSupportedFeatures".equals(method.getName())) return new String[0];
                throw new UnsupportedOperationException("Gesher's WebView does not implement "
                        + method.getDeclaringClass().getSimpleName() + "." + method.getName());
            }
            target.setAccessible(true);
            try {
                return target.invoke(delegate, args);
            } catch (InvocationTargetException e) {
                throw e.getCause();
            }
        }
    }

    /**
     * Calls the app's boundary object {@code handler}: {@code iface.name(types)} of the app's
     * copy of the boundary interface (its class loader is the handler's).
     */
    static Object call(InvocationHandler handler, String iface, String name, Class<?>[] types,
            Object... args) {
        if (handler == null) return null;
        try {
            Class<?> c = Class.forName(BOUNDARY + iface, false, handler.getClass().getClassLoader());
            Method m = c.getMethod(name, types);
            return handler.invoke(null, m, args);
        } catch (RuntimeException | Error e) {
            throw e;
        } catch (Throwable t) {
            throw new IllegalStateException(iface + "." + name + ": " + t, t);
        }
    }

    private static final Class<?>[] NONE = new Class<?>[0];

    // ---- WebViewProviderFactoryBoundaryInterface ----

    public static final class Factory {
        private final Statics mStatics = new Statics();
        private final Converter mConverter = new Converter();
        private final ProxyController mProxy = new ProxyController();

        public String[] getSupportedFeatures() {
            return FEATURES.clone();
        }

        public InvocationHandler createWebView(WebView webView) {
            Object p = webView != null ? webView.getWebViewProvider() : null;
            if (!(p instanceof GesherWebViewProvider)) {
                throw new IllegalArgumentException("not a WebView of Gesher's provider");
            }
            return Boundary.of(new WebViewProvider((GesherWebViewProvider) p));
        }

        public InvocationHandler getWebkitToCompatConverter() {
            return Boundary.of(mConverter);
        }

        public InvocationHandler getStatics() {
            return Boundary.of(mStatics);
        }

        public InvocationHandler getProxyController() {
            return Boundary.of(mProxy);
        }

        public InvocationHandler getServiceWorkerController() {
            return null;  // the framework's (API 24+)
        }

        public InvocationHandler getTracingController() {
            return null;  // the framework's (API 28+)
        }
    }

    // ---- WebViewProviderBoundaryInterface ----

    public static final class WebViewProvider {
        private final GesherWebViewProvider mProvider;

        WebViewProvider(GesherWebViewProvider provider) {
            mProvider = provider;
        }

        public void insertVisualStateCallback(long requestId, InvocationHandler callback) {
            mProvider.insertVisualStateCallback(requestId, new WebView.VisualStateCallback() {
                @Override
                public void onComplete(long id) {
                    call(callback, "VisualStateCallbackBoundaryInterface", "onComplete",
                            new Class<?>[] {long.class}, id);
                }
            });
        }

        public InvocationHandler[] createWebMessageChannel() {
            WebMessagePort[] ports = mProvider.createWebMessageChannel();
            InvocationHandler[] out = new InvocationHandler[ports.length];
            for (int i = 0; i < ports.length; i++) out[i] = Boundary.of(new Port(ports[i]));
            return out;
        }

        public void postMessageToMainFrame(InvocationHandler message, Uri targetOrigin) {
            mProvider.postMessageToMainFrame(frameworkMessage(message), targetOrigin);
        }

        public void addWebMessageListener(String jsObjectName, String[] allowedOriginRules,
                InvocationHandler listener) {
            mProvider.addMessageListener(jsObjectName, allowedOriginRules, listener);
        }

        public void removeWebMessageListener(String jsObjectName) {
            mProvider.removeMessageListener(jsObjectName);
        }

        public InvocationHandler addDocumentStartJavaScript(String script,
                String[] allowedOriginRules) {
            GesherWebViewProvider.StartScript s = mProvider.addStartScript(script,
                    allowedOriginRules);
            return Boundary.of(new ScriptHandler(mProvider, s));
        }

        public android.webkit.WebViewClient getWebViewClient() {
            return mProvider.getWebViewClient();
        }

        public android.webkit.WebChromeClient getWebChromeClient() {
            return mProvider.getWebChromeClient();
        }

        public InvocationHandler getWebViewRenderer() {
            return null;
        }

        public InvocationHandler getWebViewRendererClient() {
            return null;
        }

        public void setWebViewRendererClient(InvocationHandler client) {
        }
    }

    /** ScriptHandlerBoundaryInterface: remove() takes the script out of later documents. */
    public static final class ScriptHandler {
        private final GesherWebViewProvider mProvider;
        private final GesherWebViewProvider.StartScript mScript;

        ScriptHandler(GesherWebViewProvider provider, GesherWebViewProvider.StartScript script) {
            mProvider = provider;
            mScript = script;
        }

        public void remove() {
            mProvider.removeStartScript(mScript);
        }
    }

    /** The app's typed boundary payload as an internal framework message. */
    static WebMessage frameworkMessage(InvocationHandler message) {
        InvocationHandler[] ports = (InvocationHandler[]) call(message,
                "WebMessageBoundaryInterface", "getPorts", NONE);
        List<WebMessagePort> list = new ArrayList<>();
        if (ports != null) {
            for (InvocationHandler h : ports) {
                if (h instanceof Boundary && ((Boundary) h).delegate instanceof Port) {
                    list.add(((Port) ((Boundary) h).delegate).mPort);
                } else {
                    throw new IllegalArgumentException("not a port of Gesher's WebView");
                }
            }
        }
        WebMessagePort[] frameworkPorts = list.isEmpty() ? null : list.toArray(new WebMessagePort[0]);
        String[] supported = (String[]) call(message, "WebMessageBoundaryInterface", "getSupportedFeatures", NONE);
        if (supported != null && java.util.Arrays.asList(supported).contains("WEB_MESSAGE_ARRAY_BUFFER")) {
            InvocationHandler payload = (InvocationHandler) call(message, "WebMessageBoundaryInterface",
                    "getMessagePayload", NONE);
            int type = (Integer) call(payload, "WebMessagePayloadBoundaryInterface", "getType", NONE);
            if (type == 1) return new WebMessagePayload((byte[]) call(payload,
                    "WebMessagePayloadBoundaryInterface", "getAsArrayBuffer", NONE), frameworkPorts);
            if (type != 0) throw new IllegalArgumentException("Unknown web message payload " + type);
            return new WebMessage((String) call(payload, "WebMessagePayloadBoundaryInterface", "getAsString", NONE), frameworkPorts);
        }
        return new WebMessage((String) call(message, "WebMessageBoundaryInterface", "getData", NONE), frameworkPorts);
    }

    /** A message from the page or a port, for the app (WebMessageBoundaryInterface). */
    public static final class Message {
        private final WebMessage mMessage;
        private final InvocationHandler[] mPorts;

        Message(String data, InvocationHandler[] ports) {
            this(new WebMessage(data), ports);
        }

        Message(WebMessage message, InvocationHandler[] ports) {
            mMessage = message;
            mPorts = ports != null ? ports : new InvocationHandler[0];
        }

        public String getData() {
            if (WebMessagePayload.binary(mMessage)) throw new IllegalStateException("ArrayBuffer message");
            return mMessage.getData();
        }

        public InvocationHandler getMessagePayload() {
            return Boundary.of(new Payload(mMessage));
        }

        public InvocationHandler[] getPorts() {
            return mPorts.clone();
        }

        public String[] getSupportedFeatures() {
            return new String[] {"WEB_MESSAGE_ARRAY_BUFFER"};
        }
    }

    /** WebMessagePayloadBoundaryInterface: a string or ArrayBuffer. */
    public static final class Payload {
        private final WebMessage mMessage;

        Payload(WebMessage message) {
            mMessage = message;
        }

        public int getType() {
            return WebMessagePayload.binary(mMessage) ? 1 : 0;
        }

        public String getAsString() {
            if (getType() != 0) throw new IllegalStateException("Expected string payload");
            return mMessage.getData();
        }

        public byte[] getAsArrayBuffer() {
            byte[] bytes = WebMessagePayload.bytes(mMessage);
            if (bytes == null) throw new IllegalStateException("Expected ArrayBuffer payload");
            return bytes;
        }

        public String[] getSupportedFeatures() {
            return new String[0];
        }
    }

    /** WebMessagePortBoundaryInterface over a framework port (GesherWebMessagePort). */
    public static final class Port {
        final WebMessagePort mPort;

        Port(WebMessagePort port) {
            mPort = port;
        }

        public void postMessage(InvocationHandler message) {
            mPort.postMessage(frameworkMessage(message));
        }

        public void close() {
            mPort.close();
        }

        public void setWebMessageCallback(InvocationHandler callback) {
            setWebMessageCallback(callback, null);
        }

        public void setWebMessageCallback(InvocationHandler callback, Handler handler) {
            WebMessagePort.WebMessageCallback cb = callback == null ? null
                    : new WebMessagePort.WebMessageCallback() {
                        @Override
                        public void onMessage(WebMessagePort port, WebMessage message) {
                            InvocationHandler[] ports = new InvocationHandler[0];
                            WebMessagePort[] fp = message.getPorts();
                            if (fp != null) {
                                ports = new InvocationHandler[fp.length];
                                for (int i = 0; i < fp.length; i++) {
                                    ports[i] = Boundary.of(new Port(fp[i]));
                                }
                            }
                            call(callback, "WebMessageCallbackBoundaryInterface", "onMessage",
                                    new Class<?>[] {InvocationHandler.class, InvocationHandler.class},
                                    Boundary.of(Port.this),
                                    Boundary.of(new Message(message, ports)));
                        }
                    };
            mPort.setWebMessageCallback(cb, handler);
        }
    }

    // ---- WebMessageListener ----

    /**
     * The page posted {@code data} to the listener's object: its onPostMessage, on the
     * WebView's Looper, with the reply proxy of the page (one per document, as Chromium's).
     */
    static void deliver(GesherWebViewProvider provider, GesherWebViewProvider.MessageListener l,
            WebMessage message, String source) {
        String origin = GesherWebViewProvider.originOf(source != null ? source
                : provider.currentUrl());
        if (origin.endsWith("/")) origin = origin.substring(0, origin.length() - 1);
        Object reply = l.replyProxies.computeIfAbsent(origin,
                o -> Boundary.of(new ReplyProxy(provider, l.name)));
        try {
            call(l.listener, "WebMessageListenerBoundaryInterface", "onPostMessage",
                    new Class<?>[] {WebView.class, InvocationHandler.class, Uri.class,
                            boolean.class, InvocationHandler.class},
                    provider.view(), Boundary.of(new Message(message, null)), Uri.parse(origin),
                    true, reply);
        } catch (RuntimeException e) {
            Log.e(TAG, "WebMessageListener.onPostMessage", e);
            throw e;
        }
    }

    /** JsReplyProxyBoundaryInterface: the app's replies to one page's object. */
    public static final class ReplyProxy {
        private final GesherWebViewProvider mProvider;
        private final String mName;
        private Object mPeer;

        ReplyProxy(GesherWebViewProvider provider, String name) {
            mProvider = provider;
            mName = name;
        }

        public void postMessage(String message) {
            mProvider.postToListener(mName, message);
        }

        public void postMessageWithPayload(InvocationHandler payload) {
            int type = (Integer) call(payload, "WebMessagePayloadBoundaryInterface", "getType", NONE);
            if (type == 1) {
                mProvider.postToListener(mName, new WebMessagePayload((byte[]) call(payload,
                        "WebMessagePayloadBoundaryInterface", "getAsArrayBuffer", NONE), null));
            } else if (type == 0) {
                postMessage((String) call(payload, "WebMessagePayloadBoundaryInterface", "getAsString", NONE));
            } else {
                throw new IllegalArgumentException("Unknown web message payload " + type);
            }
        }

        /** IsomorphicObjectBoundaryInterface: one androidx peer for this proxy. */
        public synchronized Object getOrCreatePeer(Callable<Object> creator) throws Exception {
            if (mPeer == null) mPeer = creator.call();
            return mPeer;
        }
    }

    // ---- WebkitToCompatConverterBoundaryInterface ----

    public static final class Converter {
        public InvocationHandler convertSettings(WebSettings settings) {
            return Boundary.of(new Settings(settings));
        }

        public InvocationHandler convertWebResourceRequest(WebResourceRequest request) {
            return Boundary.of(new ResourceRequest(request));
        }

        public InvocationHandler convertWebMessagePort(Object port) {
            return Boundary.of(new Port((WebMessagePort) port));
        }

        public Object convertWebMessagePort(InvocationHandler port) {
            if (port instanceof Boundary && ((Boundary) port).delegate instanceof Port) {
                return ((Port) ((Boundary) port).delegate).mPort;
            }
            throw new IllegalArgumentException("not a port of Gesher's WebView");
        }
    }

    /** WebResourceRequestBoundaryInterface. */
    public static final class ResourceRequest {
        private final WebResourceRequest mRequest;

        ResourceRequest(WebResourceRequest request) {
            mRequest = request;
        }

        public boolean isRedirect() {
            return mRequest.isRedirect();
        }
    }

    /** WebSettingsBoundaryInterface over the framework's settings (GesherWebSettings). */
    public static final class Settings {
        private final WebSettings mSettings;
        private boolean mSuppressErrorPage;

        Settings(WebSettings settings) {
            mSettings = settings;
        }

        public void setOffscreenPreRaster(boolean enabled) {
            mSettings.setOffscreenPreRaster(enabled);
        }

        public boolean getOffscreenPreRaster() {
            return mSettings.getOffscreenPreRaster();
        }

        public void setSafeBrowsingEnabled(boolean enabled) {
            mSettings.setSafeBrowsingEnabled(enabled);
        }

        public boolean getSafeBrowsingEnabled() {
            return mSettings.getSafeBrowsingEnabled();
        }

        public void setDisabledActionModeMenuItems(int items) {
            mSettings.setDisabledActionModeMenuItems(items);
        }

        public int getDisabledActionModeMenuItems() {
            return mSettings.getDisabledActionModeMenuItems();
        }

        public void setWillSuppressErrorPage(boolean suppress) {
            mSuppressErrorPage = suppress;
        }

        public boolean getWillSuppressErrorPage() {
            return mSuppressErrorPage;
        }
    }

    // ---- StaticsBoundaryInterface ----

    public static final class Statics {
        public void initSafeBrowsing(Context context, ValueCallback<Boolean> callback) {
            if (callback != null) callback.onReceiveValue(true);
        }

        public void setSafeBrowsingAllowlist(Set<String> hosts, ValueCallback<Boolean> callback) {
            if (callback != null) callback.onReceiveValue(true);
        }

        public void setSafeBrowsingWhitelist(List<String> hosts, ValueCallback<Boolean> callback) {
            if (callback != null) callback.onReceiveValue(true);
        }

        public Uri getSafeBrowsingPrivacyPolicyUrl() {
            return Uri.parse("https://www.microsoft.com/privacy");
        }

        /** WebView2 renders in processes of its own. */
        public boolean isMultiProcessEnabled() {
            return true;
        }

        public String getVariationsHeader() {
            return "";
        }
    }

    // ---- ProxyControllerBoundaryInterface ----

    /**
     * androidx.webkit's ProxyController: Chromium's --proxy-server and --proxy-bypass-list
     * for the process's WebView2 environment. WebView2 takes a proxy only when its browser
     * process starts: an override set before the first WebView (or CookieManager use)
     * applies to every WebView of the process; one set later applies to the next
     * environment, which the log says.
     */
    public static final class ProxyController {
        public void setProxyOverride(String[][] proxyRules, String[] bypassRules,
                Runnable listener, Executor executor) {
            setProxyOverride(proxyRules, bypassRules, listener, executor, false);
        }

        public void setProxyOverride(String[][] proxyRules, String[] bypassRules,
                Runnable listener, Executor executor, boolean reverseBypass) {
            apply(arguments(proxyRules, bypassRules, reverseBypass), listener, executor);
        }

        public void clearProxyOverride(Runnable listener, Executor executor) {
            apply("", listener, executor);
        }

        private static void apply(String args, Runnable listener, Executor executor) {
            boolean now = WebView2Host.setProxyArguments(args);
            GesherWebView.log("ProxyController: " + (args.isEmpty() ? "the system's proxy"
                    : args) + (now ? "" : " (from the next WebView2 environment: this"
                    + " process's has started)"));
            if (listener != null) {
                if (executor != null) executor.execute(listener); else listener.run();
            }
        }

        /** Chromium's switches for Android's rules ({scheme filter, proxy URL}). */
        static String arguments(String[][] rules, String[] bypass, boolean reverse) {
            if (reverse) {
                throw new UnsupportedOperationException("reverse bypass: Chromium's"
                        + " --proxy-bypass-list has none");
            }
            // Chromium: "[scheme=]proxy[,fallback...]" per scheme, ';' between schemes.
            java.util.Map<String, StringBuilder> byScheme = new java.util.LinkedHashMap<>();
            if (rules != null) {
                for (String[] r : rules) {
                    if (r == null || r.length < 2 || r[1] == null || r[1].isEmpty()) continue;
                    String filter = r[0] == null ? "*" : r[0];
                    StringBuilder s = byScheme.computeIfAbsent(filter, f -> new StringBuilder());
                    if (s.length() > 0) s.append(',');
                    s.append(r[1]);
                }
            }
            StringBuilder servers = new StringBuilder();
            for (java.util.Map.Entry<String, StringBuilder> e : byScheme.entrySet()) {
                if (servers.length() > 0) servers.append(';');
                if (!e.getKey().equals("*")) servers.append(e.getKey()).append('=');
                servers.append(e.getValue());
            }
            StringBuilder list = new StringBuilder();
            if (bypass != null) {
                for (String b : bypass) {
                    if (b == null || b.isEmpty()) continue;
                    if (list.length() > 0) list.append(';');
                    list.append(b);
                }
            }
            StringBuilder args = new StringBuilder();
            if (servers.length() > 0) {
                args.append("--proxy-server=\"").append(servers).append('"');
            } else {
                args.append("--no-proxy-server");
            }
            if (list.length() > 0) {
                args.append(" --proxy-bypass-list=\"").append(list).append('"');
            }
            return args.toString();
        }
    }
}
