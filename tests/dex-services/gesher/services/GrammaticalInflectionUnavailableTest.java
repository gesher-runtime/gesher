/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.GrammaticalInflectionManager;
import android.content.Context;
import android.content.res.Configuration;
import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.util.List;

/** Real framework manager lookup and honest default/unavailable API behavior. */
public final class GrammaticalInflectionUnavailableTest {
    private static int checks;
    private static void require(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
        checks++;
    }
    public static void main(String[] args) throws Exception {
        String root = System.getenv("ANDROID_ROOT");
        com.gesher.app.Main.bootstrap("grammatical-test", root + "/build.prop", root + "/framework/framework-res.apk");
        com.gesher.server.Services.installInProcess(com.gesher.server.Services.createInProcess(List.of()));
        Class<?> threadType = Class.forName("android.app.ActivityThread");
        Constructor<?> constructor = threadType.getDeclaredConstructor();
        constructor.setAccessible(true);
        Method create = Class.forName("android.app.ContextImpl").getDeclaredMethod("createSystemContext", threadType);
        create.setAccessible(true);
        Context context = (Context) create.invoke(null, constructor.newInstance());
        // A system Context has no Application until ActivityThread attaches one.
        // Give the fixture the same application-context resource path as a real app.
        android.app.Application application = new android.app.Application();
        Method attach = android.app.Application.class.getDeclaredMethod("attach", Context.class);
        attach.setAccessible(true);
        attach.invoke(application, context);
        java.lang.reflect.Field packageField = context.getClass().getDeclaredField("mPackageInfo");
        packageField.setAccessible(true);
        Object loaded = packageField.get(context);
        java.lang.reflect.Field applicationField = loaded.getClass().getDeclaredField("mApplication");
        applicationField.setAccessible(true);
        applicationField.set(loaded, application);
        Object named = context.getSystemService(Context.GRAMMATICAL_INFLECTION_SERVICE);
        require(named instanceof GrammaticalInflectionManager, "Named grammatical manager is missing");
        GrammaticalInflectionManager manager = context.getSystemService(GrammaticalInflectionManager.class);
        require(manager == named, "Typed grammatical manager differs from the named manager");
        require(GrammaticalInflectionManager.class.getClassLoader() == Object.class.getClassLoader(),
                "Manager is not the actual production framework class");
        require(manager.getApplicationGrammaticalGender() == Configuration.GRAMMATICAL_GENDER_NOT_SPECIFIED,
                "Default resources invented a grammatical preference");
        require(manager.getSystemGrammaticalGender() == Configuration.GRAMMATICAL_GENDER_NOT_SPECIFIED,
                "Windows default invented a grammatical preference");
        manager.setRequestedApplicationGrammaticalGender(Configuration.GRAMMATICAL_GENDER_NOT_SPECIFIED);
        boolean unavailable = false;
        try { manager.setRequestedApplicationGrammaticalGender(Configuration.GRAMMATICAL_GENDER_FEMININE); }
        catch (UnsupportedOperationException expected) { unavailable = true; }
        require(unavailable, "Unsupported personalization reported a successful mutation");
        boolean denied = false;
        try { manager.peekSystemGrammaticalGenderByUserId(10); }
        catch (SecurityException expected) { denied = true; }
        require(denied, "Unsupported user was accepted");
        require(manager.getApplicationGrammaticalGender() == Configuration.GRAMMATICAL_GENDER_NOT_SPECIFIED,
                "Failed request changed the resource configuration");
        System.out.println("GrammaticalInflectionUnavailableTest all passed: " + checks + " checks");
    }
}
