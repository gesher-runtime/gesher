/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.ActivityThread;
import android.content.Context;
import android.content.IRestrictionsManager;
import android.content.RestrictionsManager;
import android.content.res.AssetManager;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Looper;
import android.os.PersistableBundle;
import android.os.ServiceManager;

import com.gesher.server.Services;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.util.Collections;
import java.util.Map;

/** Real ContextImpl manager construction and explicit absence of managed application policy. */
public final class ApplicationRestrictionsTest {
    private static int checks;

    private static void check(boolean condition, String reason) {
        if (!condition) throw new AssertionError(reason);
        checks++;
    }

    private interface Operation { void run() throws Exception; }

    private static void unavailable(Operation operation, String reason) throws Exception {
        try {
            operation.run();
        } catch (UnsupportedOperationException expected) {
            checks++;
            return;
        }
        throw new AssertionError(reason);
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        if (Looper.myLooper() == null) Looper.prepareMainLooper();
        Map<String, IBinder> created = Services.create(Collections.emptyList());
        IBinder binder = created.get(Context.RESTRICTIONS_SERVICE);
        check(binder != null, "restrictions service is registered");
        Field cacheField = ServiceManager.class.getDeclaredField("sCache");
        cacheField.setAccessible(true);
        @SuppressWarnings("unchecked")
        Map<String, IBinder> cache = (Map<String, IBinder>) cacheField.get(null);
        cache.putAll(created);

        Constructor<ActivityThread> constructor = ActivityThread.class.getDeclaredConstructor();
        constructor.setAccessible(true);
        Context context = constructor.newInstance().getSystemContext();
        Object lookup = context.getSystemService(Context.RESTRICTIONS_SERVICE);
        check(lookup instanceof RestrictionsManager, "real ContextImpl constructs RestrictionsManager");
        RestrictionsManager manager = (RestrictionsManager) lookup;
        check(manager == context.getSystemService(Context.RESTRICTIONS_SERVICE),
                "ContextImpl caches restrictions manager");
        check(manager == context.getSystemService(RestrictionsManager.class),
                "typed manager lookup agrees with named lookup");
        check(manager.getClass() == RestrictionsManager.class,
                "manager is the framework RestrictionsManager implementation");

        IRestrictionsManager service = IRestrictionsManager.Stub.asInterface(binder);
        check(!manager.hasRestrictionsProvider() && !service.hasRestrictionsProvider(),
                "no restrictions provider is advertised");
        Bundle restrictions = manager.getApplicationRestrictions();
        check(restrictions != null && restrictions.isEmpty(), "unmanaged policy is an empty Bundle");
        check(restrictions.getStringArray("whitelisted_google_account_names") == null,
                "no Google account allowlist is invented");
        restrictions.putString("fixture", "changed");
        check(manager.getApplicationRestrictions().isEmpty(), "caller mutation does not become policy");
        check(service.getApplicationRestrictions("com.gesher.unknown").isEmpty(),
                "unknown package reveals no policy");
        check(manager.getApplicationRestrictionsPerAdmin().isEmpty(), "no administering bundles exist");
        check(service.getApplicationRestrictionsPerAdminForUser(10, "com.gesher.unknown").isEmpty(),
                "unknown user reveals no administering bundles");
        check(manager.createLocalApprovalIntent() == null, "no local approval UI is fabricated");

        unavailable(() -> manager.requestPermission("com.gesher.synthetic.permission", "fixture-request",
                new PersistableBundle()), "permission request was accepted without a provider");
        PersistableBundle response = new PersistableBundle();
        response.putString(RestrictionsManager.REQUEST_KEY_ID, "fixture-request");
        response.putInt(RestrictionsManager.RESPONSE_KEY_RESULT, RestrictionsManager.RESULT_APPROVED);
        unavailable(() -> manager.notifyPermissionResponse("com.gesher.synthetic", response),
                "approval response was accepted without a provider");
        check(!manager.hasRestrictionsProvider() && manager.getApplicationRestrictions().isEmpty(),
                "rejected request and response did not create policy or provider state");
        System.out.println("ApplicationRestrictionsTest all passed: " + checks);
        // Services.create owns periodic workers; the isolated fixture has no app event loop.
        System.exit(0);
    }
}
