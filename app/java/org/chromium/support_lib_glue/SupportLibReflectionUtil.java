package org.chromium.support_lib_glue;

import java.lang.reflect.InvocationHandler;

/**
 * Where androidx.webkit looks for the WebView provider's support library glue
 * (WebViewGlueCommunicator: this class's {@code createWebViewProviderFactory()}, from the
 * provider's class loader). Gesher's glue is com.gesher.app.webview.SupportLib (ADR 0058).
 */
public final class SupportLibReflectionUtil {
    private SupportLibReflectionUtil() {
    }

    public static InvocationHandler createWebViewProviderFactory() {
        return com.gesher.app.webview.SupportLib.createWebViewProviderFactory();
    }
}
