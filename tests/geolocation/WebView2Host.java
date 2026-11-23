/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.app.webview;
import java.util.ArrayList;
import java.util.List;
/** Host-only message sink: no browser or native WebView2 execution. */
final class WebView2Host {
    final List<String> messages = new ArrayList<>();
    void postWebMessage(String message) { messages.add(message); }
}
