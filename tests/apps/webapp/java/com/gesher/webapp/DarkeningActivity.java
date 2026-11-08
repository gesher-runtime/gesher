package com.gesher.webapp;

import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Picture;
import android.os.Bundle;
import android.os.Handler;
import android.view.ContextThemeWrapper;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.LinearLayout;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Proxy;
import java.util.Arrays;

/** Public boundary signatures used by the affected AndroidX plugin, plus actual renderer pixels. */
public final class DarkeningActivity extends Activity {
    public interface Factory {
        String[] getSupportedFeatures();
        InvocationHandler getWebkitToCompatConverter();
    }
    public interface Converter { InvocationHandler convertSettings(WebSettings settings); }
    public interface Settings {
        void setAlgorithmicDarkeningAllowed(boolean allowed);
        boolean isAlgorithmicDarkeningAllowed();
    }
    final Handler handler = new Handler();
    final WebView[] views = new WebView[3];
    final Settings[] settings = new Settings[3];
    int loaded;
    int checks;
    int phase;
    long deadline;
    boolean done;
    static final String WHITE = "<html><head><style>html,body{margin:0;background:white;color:black}</style></head><body>fixture</body></html>";
    static final String AUTHOR = "<html><head><meta name='color-scheme' content='light dark'><style>body{margin:0;background:white;color:black}@media(prefers-color-scheme:dark){body{background:#123456;color:white}}</style></head><body>author</body></html>";
    static final String OPT_OUT = "<html><head><meta name='color-scheme' content='only light'><style>html,body{margin:0;background:white;color:black;color-scheme:only light}</style></head><body>opt out</body></html>";
    @SuppressWarnings("unchecked")
    static <T> T proxy(Class<T> type, InvocationHandler handler) {
        return (T)Proxy.newProxyInstance(type.getClassLoader(), new Class<?>[]{type}, handler);
    }
    void check(boolean good, String label) {
        if (!good) throw new AssertionError(label);
        checks++;
        System.out.println("webdark: PASS " + label);
    }
    void failure(Throwable error) {
        if (done) return;
        done = true;
        System.out.println("webdark: FAIL " + error);
        error.printStackTrace();
        finish();
    }
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        try {
            LinearLayout layout = new LinearLayout(this);
            layout.setOrientation(LinearLayout.VERTICAL);
            setContentView(layout);
            Class<?> glue = WebView.getWebViewClassLoader().loadClass("org.chromium.support_lib_glue.SupportLibReflectionUtil");
            Factory factory = proxy(Factory.class, (InvocationHandler)glue.getMethod("createWebViewProviderFactory").invoke(null));
            check(Arrays.asList(factory.getSupportedFeatures()).contains("ALGORITHMIC_DARKENING"), "provider feature advertised");
            Converter converter = proxy(Converter.class, factory.getWebkitToCompatConverter());
            for (int i = 0; i < views.length; i++) {
                final int index = i;
                ContextThemeWrapper theme = new ContextThemeWrapper(this, i == 2
                        ? android.R.style.Theme_Material_Light_NoActionBar : android.R.style.Theme_Material_NoActionBar);
                views[i] = new WebView(theme);
                views[i].getSettings().setJavaScriptEnabled(true);
                settings[i] = proxy(Settings.class, converter.convertSettings(views[i].getSettings()));
                check(!settings[i].isAlgorithmicDarkeningAllowed(), "default false view " + i);
                settings[i].setAlgorithmicDarkeningAllowed(false); // The exact failing old-plugin boundary call.
                check(!views[i].getSettings().isAlgorithmicDarkeningAllowed(), "boundary false view " + i);
                layout.addView(views[i], new LinearLayout.LayoutParams(-1, 200));
                views[i].setWebViewClient(new WebViewClient() {
                    @Override public void onPageFinished(WebView view, String url) {
                        if (phase == 0) { if (++loaded == 3) verifySchemes(0); }
                        else if (index == 0) poll();
                    }
                });
                views[i].loadDataWithBaseURL("https://fixture.invalid/", WHITE, "text/html", "UTF-8", null);
            }
            handler.postDelayed(() -> { if (!done) failure(new AssertionError("fixture timed out")); }, 45000);
        } catch (Throwable error) { failure(error); }
    }
    void verifySchemes(int index) {
        if (index == views.length) {
            phase = 1;
            deadline = android.os.SystemClock.uptimeMillis() + 8000;
            poll();
            return;
        }
        views[index].evaluateJavascript("JSON.stringify([matchMedia('(prefers-color-scheme:dark)').matches,document.documentElement.style.colorScheme])", result -> {
            try {
                String expected = index == 2 ? "false" : "true";
                check(result.contains(expected) && result.contains("\\\"\\\""), "theme media without root style mutation view " + index);
                verifySchemes(index + 1);
            } catch (Throwable error) { failure(error); }
        });
    }
    int pixel(int index) {
        Picture picture = views[index].capturePicture();
        Bitmap bitmap = Bitmap.createBitmap(Math.max(1, views[index].getWidth()), Math.max(1, views[index].getHeight()), Bitmap.Config.ARGB_8888);
        picture.draw(new Canvas(bitmap));
        int color = bitmap.getPixel(bitmap.getWidth() / 2, bitmap.getHeight() / 2) & 0xffffff;
        bitmap.recycle();
        return color;
    }
    boolean near(int value, int expected) {
        return Math.abs((value >> 16) - (expected >> 16)) < 24
                && Math.abs(((value >> 8) & 255) - ((expected >> 8) & 255)) < 24
                && Math.abs((value & 255) - (expected & 255)) < 24;
    }
    void poll() {
        if (done) return;
        try {
            if (deadline == 0) deadline = android.os.SystemClock.uptimeMillis() + 8000;
            int a = pixel(0), b = pixel(1), c = pixel(2);
            boolean ready = phase == 2 ? a < 0x808080 : phase == 4 ? near(a, 0x123456) : near(a, 0xffffff);
            ready &= near(b, 0xffffff) && near(c, 0xffffff);
            if (!ready) {
                if (android.os.SystemClock.uptimeMillis() >= deadline)
                    throw new AssertionError("phase " + phase + " pixels " + Integer.toHexString(a) + "/" + Integer.toHexString(b) + "/" + Integer.toHexString(c));
                handler.postDelayed(this::poll, 200);
                return;
            }
            check(true, "renderer phase " + phase + " pixels " + Integer.toHexString(a) + "/" + Integer.toHexString(b) + "/" + Integer.toHexString(c));
            deadline = 0;
            if (phase == 1) {
                settings[0].setAlgorithmicDarkeningAllowed(true);
                settings[2].setAlgorithmicDarkeningAllowed(true);
                check(settings[0].isAlgorithmicDarkeningAllowed() && !settings[1].isAlgorithmicDarkeningAllowed()
                        && settings[2].isAlgorithmicDarkeningAllowed(), "independent boundary true and false state");
                phase = 2;
            } else if (phase == 2) {
                settings[0].setAlgorithmicDarkeningAllowed(false);
                check(!settings[0].isAlgorithmicDarkeningAllowed(), "boundary disable reverses state");
                phase = 3;
            } else if (phase == 3) {
                settings[0].setAlgorithmicDarkeningAllowed(true);
                phase = 4;
                views[0].loadDataWithBaseURL("https://fixture.invalid/author", AUTHOR, "text/html", "UTF-8", null);
                return;
            } else if (phase == 4) {
                phase = 5;
                views[0].loadDataWithBaseURL("https://fixture.invalid/opt-out", OPT_OUT, "text/html", "UTF-8", null);
                return;
            } else {
                done = true;
                System.out.println("webdark: ALL PASSED " + checks);
                finish();
                return;
            }
            handler.postDelayed(this::poll, 250);
        } catch (Throwable error) { failure(error); }
    }
    @Override protected void onDestroy() {
        done = true;
        for (WebView view : views) if (view != null) view.destroy();
        super.onDestroy();
    }
}
