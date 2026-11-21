#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Exercise exact WebView document publication/answering and measure owned-body copies.

Copies production loadDataWithBaseURL, answerRequest, blocked, readAll, headers,
navigation, onNavigationStarting, pending-data state and ApiNavigations. Native host/JNI,
the UI thread and file-resource loading are declared boundaries; file/byte streams,
concurrency and Java allocations are real.
--framework-classes runs real Android WebResourceResponse/Base64 implementations;
otherwise small declared Android adapters keep this host preflight standalone.
Only the pending-data read boundary is instrumented to force concurrent changes.
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
    public static byte[] decode(String value, int flags) {
        return java.util.Base64.getMimeDecoder().decode(value);
    }
}
'''


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home', type=Path, required=True)
    parser.add_argument('--framework-classes', type=Path)
    parser.add_argument('--source-ref')
    parser.add_argument('--case', choices=('all', 'behavior', 'concurrency', 'measurement'), default='all')
    parser.add_argument('--measure', action='store_true', help='omit new allocation/identity gates')
    args = parser.parse_args()

    def read(path: str) -> str:
        if args.source_ref:
            return subprocess.run(['git', 'show', args.source_ref + ':' + path], cwd=ROOT,
                                  capture_output=True, text=True, check=True).stdout
        return (ROOT / path).read_text(encoding='utf-8')

    production = read('app/java/com/gesher/app/webview/GesherWebViewProvider.java')
    pieces = [method(production, marker) for marker in (
        'public void loadDataWithBaseURL(String baseUrl, String data, String mimeType,',
        'private void navigate(String webView2Url, String headers)',
        'private void navigate(String webView2Url, String headers, PendingData data)',
        'public boolean onNavigationStarting(String url, boolean userInitiated, boolean redirect)',
        'private void answerRequest(long id, String url, String method, String headers, int context,',
        'private boolean blocked(String url, int context)',
        'private static byte[] readAll(InputStream in)',
        'private static Map<String, String> parseHeaders(String headers)',
    )]
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
