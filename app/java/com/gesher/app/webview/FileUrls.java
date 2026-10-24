package com.gesher.app.webview;

import android.content.Context;
import android.content.res.Resources;
import android.net.Uri;
import android.util.TypedValue;
import android.webkit.MimeTypeMap;
import android.webkit.WebResourceResponse;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

/**
 * file:/// URLs in a WebView2 (docs/webview.md, "URLs"). WebView2 neither serves an APK's
 * assets nor lets the host intercept file: requests, so file:///x is loaded as
 * {@code https://gesher-file.example/x} and every request there is answered by Gesher:
 * file:///android_asset/ from the APK's assets, file:///android_res/ from its resources and
 * other paths from the disk when WebSettings allows file access. Everything the app sees
 * (getUrl, WebViewClient's callbacks, WebResourceRequest) carries the file:/// URL.
 */
public final class FileUrls {
    /** compat/webview/webview_host.h's kFileOrigin. */
    public static final String ORIGIN = "https://gesher-file.example";
    private static final String FILE = "file:///";

    private FileUrls() {
    }

    /** The URL WebView2 loads for an app's URL. */
    public static String toWebView2(String url) {
        if (url != null && url.regionMatches(true, 0, FILE, 0, FILE.length())) {
            return ORIGIN + "/" + url.substring(FILE.length());
        }
        return url;
    }

    /** The app's URL for one WebView2 reports. */
    public static String fromWebView2(String url) {
        if (url != null && url.startsWith(ORIGIN + "/")) {
            return FILE + url.substring(ORIGIN.length() + 1);
        }
        return url;
    }

    public static boolean isFile(String webView2Url) {
        return webView2Url != null && webView2Url.startsWith(ORIGIN + "/");
    }

    /**
     * The response for a file:/// URL: the asset, resource or file, or a 404 response
     * when it is missing or not allowed.
     */
    public static WebResourceResponse load(Context context, String fileUrl,
            boolean allowFileAccess) {
        String path = Uri.parse(fileUrl).getPath();
        if (path == null) path = "/";
        InputStream in = null;
        try {
            if (path.startsWith("/android_asset/")) {
                in = context.getAssets().open(path.substring("/android_asset/".length()));
            } else if (path.startsWith("/android_res/")) {
                in = openResource(context, path.substring("/android_res/".length()));
            } else if (allowFileAccess) {
                File f = new File(path);
                if (f.isFile()) in = new FileInputStream(f);
            }
        } catch (Exception e) {
            in = null;
        }
        if (in == null) {
            return response("text/plain", 404, "Not Found",
                    new ByteArrayInputStream(new byte[0]));
        }
        return response(mimeType(path), 200, "OK", in);
    }

    private static WebResourceResponse response(String mime, int status, String reason,
            InputStream data) {
        Map<String, String> headers = new HashMap<>();
        headers.put("Access-Control-Allow-Origin", "*");
        headers.put("Cache-Control", "no-cache");
        return new WebResourceResponse(mime, null, status, reason, headers, data);
    }

    /** android_res/<type>/<name>.<ext>: the resource of that type and name. */
    private static InputStream openResource(Context context, String rest) {
        int slash = rest.indexOf('/');
        if (slash < 0) return null;
        String type = rest.substring(0, slash);
        String name = rest.substring(slash + 1);
        int dot = name.lastIndexOf('.');
        if (dot > 0) name = name.substring(0, dot);
        // "drawable-hdpi" and the like name the type too.
        int dash = type.indexOf('-');
        if (dash > 0) type = type.substring(0, dash);
        Resources res = context.getResources();
        int id = res.getIdentifier(name, type, context.getPackageName());
        if (id == 0) return null;
        return res.openRawResource(id, new TypedValue());
    }

    private static final Map<String, String> MIME = new HashMap<>();

    static {
        MIME.put("html", "text/html");
        MIME.put("htm", "text/html");
        MIME.put("xhtml", "application/xhtml+xml");
        MIME.put("js", "text/javascript");
        MIME.put("mjs", "text/javascript");
        MIME.put("css", "text/css");
        MIME.put("json", "application/json");
        MIME.put("txt", "text/plain");
        MIME.put("xml", "text/xml");
        MIME.put("svg", "image/svg+xml");
        MIME.put("png", "image/png");
        MIME.put("jpg", "image/jpeg");
        MIME.put("jpeg", "image/jpeg");
        MIME.put("gif", "image/gif");
        MIME.put("webp", "image/webp");
        MIME.put("ico", "image/x-icon");
        MIME.put("woff", "font/woff");
        MIME.put("woff2", "font/woff2");
        MIME.put("ttf", "font/ttf");
        MIME.put("otf", "font/otf");
        MIME.put("wasm", "application/wasm");
        MIME.put("mp3", "audio/mpeg");
        MIME.put("mp4", "video/mp4");
        MIME.put("webm", "video/webm");
    }

    static String mimeType(String path) {
        int dot = path.lastIndexOf('.');
        String ext = dot >= 0 ? path.substring(dot + 1).toLowerCase(Locale.ROOT) : "";
        String mime = MIME.get(ext);
        if (mime == null) {
            try {
                mime = MimeTypeMap.getSingleton().getMimeTypeFromExtension(ext);
            } catch (RuntimeException e) {
                mime = null;
            }
        }
        return mime != null ? mime : "application/octet-stream";
    }
}
