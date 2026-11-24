/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.webkit.WebView;
import android.webkit.WebViewProviderInfo;
import android.webkit.WebViewProviderResponse;

import com.gesher.app.webview.WebView2Host;
import com.gesher.server.WebViewUpdateService;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.List;

/**
 * WebView.getCurrentWebViewPackage() through the production framework and the
 * webviewupdate service: the built-in provider at the installed WebView2 Runtime's
 * version (Capacitor reads its versionName before loading a WebView), null without one.
 */
public final class WebViewProviderPackageTest {
    private static int checks;

    private static void require(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
        checks++;
    }

    public static void main(String[] args) throws Exception {
        String root = System.getenv("ANDROID_ROOT");
        com.gesher.app.Main.bootstrap("webview-package-test", root + "/build.prop",
                root + "/framework/framework-res.apk");
        com.gesher.server.Services.installInProcess(
                com.gesher.server.Services.createInProcess(List.of()));
        // bindApplication hands an app process the device's features; do the same here.
        android.content.pm.IPackageManager packages = android.content.pm.IPackageManager.Stub
                .asInterface(android.os.ServiceManager.getService("package"));
        android.content.pm.SystemFeaturesCache.setInstance(new android.content.pm.SystemFeaturesCache(
                packages.getSystemAvailableFeatures().getList()));
        // WebViewFactory.isWebViewSupported() asks the initial Application's PackageManager,
        // as in an app process: give the fixture an ActivityThread with one.
        Class<?> threadType = Class.forName("android.app.ActivityThread");
        Constructor<?> constructor = threadType.getDeclaredConstructor();
        constructor.setAccessible(true);
        Object thread = constructor.newInstance();
        Method create = Class.forName("android.app.ContextImpl")
                .getDeclaredMethod("createSystemContext", threadType);
        create.setAccessible(true);
        Context context = (Context) create.invoke(null, thread);
        android.app.Application application = new android.app.Application();
        Method attach = android.app.Application.class.getDeclaredMethod("attach", Context.class);
        attach.setAccessible(true);
        attach.invoke(application, context);
        Field initial = threadType.getDeclaredField("mInitialApplication");
        initial.setAccessible(true);
        initial.set(thread, application);
        Field current = threadType.getDeclaredField("sCurrentActivityThread");
        current.setAccessible(true);
        current.set(null, thread);

        require(context.getPackageManager().hasSystemFeature(PackageManager.FEATURE_WEBVIEW),
                "The device does not declare its built-in WebView");

        String runtime = WebView2Host.runtimeVersion();
        PackageInfo info = WebView.getCurrentWebViewPackage();
        if (runtime == null) {
            require(info == null, "A WebView package was reported without a WebView2 Runtime");
        } else {
            require(info != null, "No WebView package although WebView2 " + runtime + " is installed");
            require(WebViewUpdateService.PROVIDER_PACKAGE.equals(info.packageName),
                    "Unexpected provider package " + info.packageName);
            require(runtime.equals(info.versionName), "versionName " + info.versionName
                    + " is not the runtime's " + runtime);
            int major = Integer.parseInt(info.versionName.split("\\.")[0]);
            require(major >= 60, "Implausible Chromium major version " + major);
            require(info.getLongVersionCode() > 0, "No version code for " + info.versionName);
            require(info.applicationInfo != null
                    && WebViewUpdateService.PROVIDER_PACKAGE.equals(info.applicationInfo.packageName),
                    "The provider's ApplicationInfo is missing");
        }

        // The service's own answers, with and without a runtime (no host dependency); the
        // package manager only serves changeProviderAndSetting's permission check.
        com.gesher.server.PackageManagerService pm = null;
        WebViewUpdateService present = new WebViewUpdateService(pm, () -> "158.0.4370.33");
        PackageInfo known = present.getCurrentWebViewPackage();
        require(known != null && "158.0.4370.33".equals(known.versionName), "Runtime version not reported");
        require(known.getLongVersionCode() == 437003300L, "Unexpected version code "
                + known.getLongVersionCode());
        require(WebViewUpdateService.PROVIDER_PACKAGE.equals(present.getCurrentWebViewPackageName()),
                "Current package name differs from the package");
        WebViewProviderResponse response = present.waitForAndGetProvider();
        require(response.status == WebViewProviderResponse.STATUS_SUCCESS
                && response.packageInfo != null, "Provider response is not a success");
        require(present.getValidWebViewPackages().length == 1, "The installed provider is not valid");

        WebViewUpdateService absent = new WebViewUpdateService(pm, () -> null);
        require(absent.getCurrentWebViewPackage() == null, "A package was invented without a runtime");
        require(absent.getCurrentWebViewPackageName() == null, "A name was invented without a runtime");
        require(absent.waitForAndGetProvider().status
                == WebViewProviderResponse.STATUS_FAILED_LISTING_WEBVIEW_PACKAGES,
                "Missing runtime reported as a provider");
        require(absent.getValidWebViewPackages().length == 0, "A missing provider was listed as valid");
        WebViewProviderInfo[] all = absent.getAllWebViewPackages();
        require(all.length == 1 && WebViewUpdateService.PROVIDER_PACKAGE.equals(all[0].packageName),
                "The configured provider is not listed");
        System.out.println("WebViewProviderPackageTest all passed: " + checks + " checks");
    }
}
