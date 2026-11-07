/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.sdksandbox.SdkSandboxManager;
import android.content.Context;
import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.util.List;

/** Optional SDK capability is queryable without advertising an isolated runtime. */
public final class SdkSandboxUnsupportedTest {
    private static int checks;
    private static void require(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
        ++checks;
    }
    public static void main(String[] args) throws Exception {
        String root = System.getenv("ANDROID_ROOT");
        com.gesher.app.Main.bootstrap("sdk-sandbox-test", root + "/build.prop", root + "/framework/framework-res.apk");
        com.gesher.server.Services.installInProcess(com.gesher.server.Services.create(List.of()));
        Class<?> threadType = Class.forName("android.app.ActivityThread");
        Constructor<?> constructor = threadType.getDeclaredConstructor();
        constructor.setAccessible(true);
        Object thread = constructor.newInstance();
        Method create = Class.forName("android.app.ContextImpl").getDeclaredMethod("createSystemContext", threadType);
        create.setAccessible(true);
        Context context = (Context) create.invoke(null, thread);
        // The original Store collector calls this optional-module static method.
        require(SdkSandboxManager.getSdkSandboxState() == SdkSandboxManager.SDK_SANDBOX_STATE_DISABLED,
                "Unavailable SDK sandbox reported isolated execution support");
        require(SdkSandboxManager.SDK_SANDBOX_STATE_DISABLED == 0
                        && SdkSandboxManager.SDK_SANDBOX_STATE_ENABLED_PROCESS_ISOLATION == 2,
                "SDK sandbox capability constants differ from Android");
        require(context.getSystemService("sdk_sandbox") == null, "Named lookup fabricated an SDK sandbox service");
        require(context.getSystemService(SdkSandboxManager.class) == null, "Typed lookup fabricated an SDK sandbox service");
        require(SdkSandboxManager.class.getClassLoader() == Object.class.getClassLoader(),
                "SDK sandbox capability type is not the production boot class");
        require(SdkSandboxManager.class.getConstructors().length == 0, "Unsupported manager is publicly constructible");
        System.out.println("SdkSandboxUnsupportedTest all passed: " + checks + " synthetic checks");
    }
}
