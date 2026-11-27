#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Exercise exact WebView document publication/answering and measure owned-body copies.

Copies production load/navigation/stop/history/destroy methods, onResourceRequest,
claimData, answerRequest, blocked, readAll, headers, onEvent, onNavigationStarting,
pending-data state and ApiNavigations. Native
host/JNI (a model of webview_host.cc's navigation events), the UI thread and file-resource
loading are declared boundaries; file/byte streams, concurrency and Java allocations are
real.
--framework-classes runs real Android WebResourceResponse/Base64 implementations;
adding --core-classes runs the real IntentFallback/Intent parser too. Otherwise a
finite URL mapping is a declared parser boundary, not another parser. The UI queue
and wait duration are controlled for lifecycle checks. Only the pending-data read
boundary is instrumented inside the extracted production code.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess
import tempfile

from test_install_metadata_comparison import method

ROOT = Path(__file__).resolve().parents[1]

RESPONSE_ADAPTER = r'''
package android.webkit;
import java.io.InputStream;
import java.util.Map;
public class WebResourceResponse {
    private final String mime, encoding;
    private final InputStream data;
    private int status;
    private String reason;
    private Map<String,String> headers;
    public WebResourceResponse(String mime, String encoding, InputStream data) {
        this.mime = mime; this.encoding = encoding; this.data = data;
    }
    public WebResourceResponse(String mime, String encoding, int status, String reason,
            Map<String,String> headers, InputStream data) {
        this(mime, encoding, data);
        this.status = status; this.reason = reason; this.headers = headers;
    }
    public String getMimeType() { return mime; }
    public String getEncoding() { return encoding; }
    public InputStream getData() { return data; }
    public int getStatusCode() { return status; }
    public String getReasonPhrase() { return reason; }
    public Map<String,String> getResponseHeaders() { return headers; }
}
'''
BASE64_ADAPTER = r'''
package android.util;
public final class Base64 {
    public static final int DEFAULT = 0;
    public static final int NO_WRAP = 2;
    public static byte[] decode(String value, int flags) {
        return java.util.Base64.getMimeDecoder().decode(value);
    }
    public static String encodeToString(byte[] value, int flags) {
        return java.util.Base64.getEncoder().encodeToString(value);
    }
}
'''
INTENT_FALLBACK_BOUNDARY = r'''
package com.gesher.app.webview;
/** One declared parser result for lifecycle checks; no Intent URI parsing here. */
final class IntentFallback {
    static String httpUrl(String url) {
        return WebViewOwnedResponsesHost.INTENT_URL.equals(url)
                ? WebViewOwnedResponsesHost.FALLBACK_URL : null;
    }
}
'''


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home', type=Path, required=True)
    parser.add_argument('--framework-classes', type=Path)
    parser.add_argument('--core-classes', type=Path)
    parser.add_argument('--source-ref')
    parser.add_argument('--case', choices=('all', 'behavior', 'concurrency', 'measurement'), default='all')
    parser.add_argument('--measure', action='store_true', help='omit new allocation/identity gates')
    args = parser.parse_args()
    if args.core_classes and not args.framework_classes:
        parser.error('--core-classes requires --framework-classes')

    def read(path: str) -> str:
        if args.source_ref:
            return subprocess.run(['git', 'show', args.source_ref + ':' + path], cwd=ROOT,
                                  capture_output=True, text=True, check=True).stdout
        return (ROOT / path).read_text(encoding='utf-8')

    production = read('app/java/com/gesher/app/webview/GesherWebViewProvider.java')
    pieces = [method(production, marker) for marker in (
        'public void loadUrl(String url, Map<String, String> additionalHttpHeaders)',
        'public void loadUrl(String url)',
        'public void postUrl(String url, byte[] postData)',
        'public void loadData(String data, String mimeType, String encoding)',
        'public void loadDataWithBaseURL(String baseUrl, String data, String mimeType,',
        'public void stopLoading()',
        'public void reload()',
        'public void goBack()',
        'public void goForward()',
        'public void goBackOrForward(int steps)',
        'public void destroy()',
        'private void navigate(String webView2Url, String headers)',
        'private void navigate(String webView2Url, String headers, PendingData data)',
        'public void onEvent(int type, long id, int arg, String a, String b, String c, byte[] bytes)',
        'public boolean onNavigationStarting(String url, boolean userInitiated, boolean redirect',
        'public void onResourceRequest(long id, String url, String method, String headers,',
        'private void answerRequest(long id, String url, String method, String headers, int context,',
        'private boolean blocked(String url, int context)',
        'private static byte[] readAll(InputStream in)',
        'private static Map<String, String> parseHeaders(String headers)',
    )]
    # Helpers newer than an old-source control's production code.
    pieces += [method(production, marker) for marker in (
        'private static long number(String decimal)',
        'private PendingData claimData(String url, int context, boolean mainFrame)',
        'private boolean canUseIntentFallback(long generation)',
    ) if marker in production]
    for field in ('private final AtomicLong mNavigationGeneration = new AtomicLong();',
                  'private boolean mDestroyed;'):
        if field in production:
            if production.count(field) != 1:
                raise ValueError('navigation lifecycle state changed: ' + field)
            pieces.append(field)
    # The original PR had a provider-local parser. Old-source controls use the
    # same declared parsing boundary as the repaired provider, retaining its
    # exact navigation/callback/scheduling code.
    if 'private static String intentFallbackUrl(String url)' in production:
        pieces.append('private static String intentFallbackUrl(String url) { '
                      'return IntentFallback.httpUrl(url); }')
    if 'private static final class PendingData' in production:
        fields = ['private final AtomicReference<PendingData> mPendingData = new AtomicReference<>();',
                  'private final ApiNavigations mApiNavigations = new ApiNavigations();',
                  'private boolean mStartingApi;']
        for field in fields:
            if production.count(field) != 1:
                raise ValueError('pending document state changed: ' + field)
        pieces += [method(production, 'private static final class PendingData'), *fields]
        inspections = {
            '@BODY@': 'PendingData value = mPendingData.get(); return value == null ? null : value.body;',
            '@MIME@': 'PendingData value = mPendingData.get(); return value == null ? null : value.mime;',
            '@URL@': 'PendingData value = mPendingData.get(); return value == null ? null : value.url;',
        }
        atomic = True
    else:
        for name in ('mDataUrl', 'mData', 'mDataMime'):
            matches = re.findall(r'private volatile (?:String|byte\[\]) ' + name + r';', production)
            if len(matches) != 1:
                raise ValueError('legacy pending document field changed: ' + name)
            pieces += matches
        inspections = {'@BODY@': 'return mData;', '@MIME@': 'return mDataMime;', '@URL@': 'return mDataUrl;'}
        atomic = False
    exact = '\n'.join(pieces)
    if 'private void traceNavigation(String message)' in production:
        trace_flag = re.findall(r'private static final boolean TRACE_NAVIGATION\s*=\s*'
                               r'"1"\.equals\(System.getenv\("GESHER_TRACE_WEBVIEW_NAVIGATION"\)\);',
                               production)
        if len(trace_flag) != 1:
            raise ValueError('navigation diagnostic flag changed')
        pieces += [method(production, 'private void traceNavigation(String message)'),
                   trace_flag[0]]
        exact = '\n'.join(pieces)
    print('WebView response production SHA256 ' + hashlib.sha256(exact.encode()).hexdigest(), flush=True)
    if atomic:
        boundary = 'PendingData data = mPendingData.get();'
        if exact.count(boundary) != 1:
            raise ValueError('pending-document read boundary changed')
        exact = exact.replace(boundary, 'PendingData data = fixtureRead(mPendingData);')
    else:
        boundary = 'mData = null;'
        if exact.count(boundary) != 1:
            raise ValueError('legacy pending-document claim boundary changed')
        exact = exact.replace(boundary, 'fixtureBeforeClaim();\n                ' + boundary)
    file_urls = read('app/java/com/gesher/app/webview/FileUrls.java')
    mappings = '\n'.join(method(file_urls, marker) for marker in (
        'public static String toWebView2(String url)',
        'public static String fromWebView2(String url)',
        'public static boolean isFile(String webView2Url)',
    ))
    template = (ROOT / 'tests/java-performance/WebViewOwnedResponsesHost.java.in').read_text(encoding='utf-8')
    generated = template.replace('@PRODUCTION@', exact).replace('@URL_MAPPINGS@', mappings)
    generated = generated.replace('@NETWORK_CLEAR_GATE@', str('mNetworkErrors.clear();' in exact).lower())
    # The native callback is a declared boundary in this Java test. Preserve its
    # old matching behavior for old-source controls; the actual C++ helper has
    # its own executable regression tests.
    native = read('compat/webview/webview_host.cc')
    native_match = ('ApiNavigations.document(url).equals(ApiNavigations.document(mainDocument))'
                    if 'SameDocumentUrl(' in native else 'url.equals(mainDocument)')
    generated = generated.replace('@NATIVE_DOCUMENT_MATCH@', native_match)
    generated = generated.replace('@STARTING_HANDED@',
        ', handed' if 'boolean redirect, long handed)' in production else '')
    generated = generated.replace('@DIRECT_HANDED@',
        ', 0' if 'boolean redirect, long handed)' in production else '')
    for marker, value in inspections.items():
        generated = generated.replace(marker, value)
    if re.search(r'@[A-Z_]+@', generated):
        raise ValueError('unexpanded WebView fixture boundary')
    with tempfile.TemporaryDirectory(prefix='gesher-webview-owned-') as temporary:
        work = Path(temporary)
        source = work / 'WebViewOwnedResponsesHost.java'
        source.write_text(generated, encoding='utf-8')
        navigations = work / 'ApiNavigations.java'
        navigations.write_text(read('app/java/com/gesher/app/webview/ApiNavigations.java'), encoding='utf-8')
        sources = [str(source), str(navigations)]
        classpath = [str(work)]
        if args.framework_classes:
            classpath.append(str(args.framework_classes.resolve()))
            print('Android metadata/Base64: actual framework classes', flush=True)
        else:
            for name, adapter in [('WebResourceResponse', RESPONSE_ADAPTER), ('Base64', BASE64_ADAPTER)]:
                path = work / (name + '.java'); path.write_text(adapter, encoding='utf-8'); sources.append(str(path))
            print('Android metadata/Base64: declared portable adapters', flush=True)
        fallback = work / 'IntentFallback.java'
        if args.core_classes:
            classpath.append(str(args.core_classes.resolve()))
            fallback.write_text(read('app/java/com/gesher/app/webview/IntentFallback.java'), encoding='utf-8')
            print('Intent fallback: actual production helper and Android parser', flush=True)
        else:
            fallback.write_text(INTENT_FALLBACK_BOUNDARY, encoding='utf-8')
            print('Intent fallback: declared finite mapping boundary (not a parser test)', flush=True)
        sources.append(str(fallback))
        cp = os.pathsep.join(classpath)
        subprocess.run([str(args.java_home / 'bin/javac'), '-J-XX:-UsePerfData', '-source', '11',
                        '-target', '11', '-Xlint:-options', '-encoding', 'UTF-8', '-cp', cp,
                        '-d', str(work), *sources], check=True, timeout=60)
        for case in ['behavior', 'concurrency', 'measurement'] if args.case == 'all' else [args.case]:
            subprocess.run([str(args.java_home / 'bin/java'), '-XX:-UsePerfData', '-Xmx128m',
                            '-cp', cp, 'com.gesher.app.webview.WebViewOwnedResponsesHost', case,
                            'measure' if args.measure else 'gate'], check=True, timeout=90)


if __name__ == '__main__':
    main()
