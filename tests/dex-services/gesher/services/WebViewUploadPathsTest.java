/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import com.gesher.app.webview.WebView2Host;
import com.gesher.server.Zygote;
import java.util.function.Function;

/** File-chooser path conversion; browser upload/network behavior is separate. */
public final class WebViewUploadPathsTest {
    private static int checks;

    private static void equal(String expected, String actual) {
        if (!expected.equals(actual)) throw new AssertionError("Expected " + expected + ", got " + actual);
        checks++;
    }

    public static void contracts(Function<String, String> convert) {
        for (String drive : new String[]{"a", "c", "d", "z"}) {
            String windows = drive.toUpperCase(java.util.Locale.ROOT) + ":\\upload folder\\קובץ 😀.txt";
            equal(windows, convert.apply("/mnt/" + drive + "/upload folder/קובץ 😀.txt"));
            equal(windows, convert.apply(windows));
            equal(windows, convert.apply(windows.replace('\\', '/')));
            equal(windows, convert.apply("\\\\?\\" + windows));
        }
        String unc = "\\\\server\\share\\upload folder\\קובץ.txt";
        equal(unc, convert.apply("/mnt/unc/server/share/upload folder/קובץ.txt"));
        equal(unc, convert.apply(unc));
        equal(unc, convert.apply(unc.replace('\\', '/')));
        equal(unc, convert.apply("\\\\?\\UNC\\" + unc.substring(2)));
        equal(Zygote.windowsPath("/ordinary/file.txt"), convert.apply("/ordinary/file.txt"));
        equal(Zygote.windowsPath("relative/file.txt"), convert.apply("relative/file.txt"));
        equal(Zygote.windowsPath("/mnt/cousin/file.txt"), convert.apply("/mnt/cousin/file.txt"));
        for (String invalid : new String[]{"/mnt/unc", "/mnt/unc/server", "\\\\.\\pipe\\name"}) {
            boolean rejected = false;
            try { convert.apply(invalid); }
            catch (IllegalArgumentException expected) { rejected = true; }
            if (!rejected) throw new AssertionError("Unsupported namespace accepted: " + invalid);
            checks++;
        }
        System.out.println("WebViewUploadPathsTest all passed: " + checks + " checks");
    }

    public static void main(String[] args) {
        System.loadLibrary("android_runtime");
        if (!WebView2Host.load()) throw new AssertionError("WebView bridge did not load");
        contracts(WebView2Host::windowsPath);
    }
}
