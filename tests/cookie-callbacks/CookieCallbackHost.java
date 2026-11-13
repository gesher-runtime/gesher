/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
public final class CookieCallbackHost {
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        com.gesher.app.webview.WebView2Host.available = !args[1].equals("unavailable");
        com.gesher.app.webview.WebView2Host.immediate = args[1].equals("immediate");
        gesher.services.CookieCallbackContractTest.contracts(
                com.gesher.app.webview.WebView2Host.available);
        System.out.println("CookieCallback host " + args[1] + " passed: "
                + gesher.services.CookieCallbackContractTest.checks + " checks");
    }
}
