package com.gesher.bindersg;

import android.app.Activity;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import java.lang.reflect.Method;
import java.util.Objects;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

public final class SgActivity extends Activity {
    private static native int run(String role, String scenario, String prefix, String endpoint);
    private static native String currentEndpoint();
    private String verdict = "FAILED";
    private static String required(String key) {
        String value = android.system.Os.getenv(key);
        if (value == null || value.isEmpty()) throw new IllegalArgumentException("missing " + key);
        return value;
    }
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        AtomicBoolean running = new AtomicBoolean(true);
        AtomicInteger pings = new AtomicInteger();
        AtomicReference<Throwable> failure = new AtomicReference<>();
        Thread observer = null;
        try {
            System.loadLibrary("bindersg");
            String role = getPackageName().substring(getPackageName().lastIndexOf('.') + 1);
            if (!role.equals("server") && !role.equals("client")) throw new AssertionError("separate server/client APK identity required");
            String scenario = required("GESHER_SG_SCENARIO");
            String prefix = required("GESHER_SG_PREFIX");
            String endpoint = required("GESHER_SG_DEVICE");
            String mode = required("GESHER_SG_MODE");
            Method checkService = Class.forName("android.os.ServiceManager").getDeclaredMethod("checkService", String.class);
            final IBinder framework = (IBinder) checkService.invoke(null, "package");
            if (framework == null || !framework.pingBinder()) throw new AssertionError("existing package Binder is unavailable");
            if (mode.equals("device") && framework instanceof Binder) throw new AssertionError("device package Binder must be remote");
            final String descriptor = framework.getInterfaceDescriptor();
            final String before = currentEndpoint();
            System.out.println("SG APK identity: pid=" + android.os.Process.myPid() + "; role=" + role
                    + "; mode=" + mode + "; framework remote=" + !(framework instanceof Binder));
            CountDownLatch started = new CountDownLatch(1);
            observer = new Thread(() -> {
                started.countDown();
                try {
                    while (running.get()) {
                        if (!framework.pingBinder() || !descriptor.equals(framework.getInterfaceDescriptor())) {
                            throw new AssertionError("established framework Binder changed during native actor");
                        }
                        pings.incrementAndGet();
                        Thread.sleep(2);
                    }
                } catch (Throwable error) { failure.compareAndSet(null, error); }
            }, "sg-existing-framework-session");
            observer.setDaemon(true);
            observer.start();
            if (!started.await(5, TimeUnit.SECONDS)) throw new AssertionError("framework observer did not start");
            int status = run(role, scenario, prefix, endpoint);
            boolean restored = Objects.equals(before, currentEndpoint());
            if (status != 0 || !restored || framework != checkService.invoke(null, "package") || !framework.pingBinder()) {
                throw new AssertionError("actor/status/endpoint/framework identity failed: " + status + "/" + restored);
            }
            running.set(false);
            observer.join(5000);
            if (observer.isAlive() || failure.get() != null || pings.get() == 0) throw new AssertionError("framework observer failed", failure.get());
            verdict = "all passed; role=" + role + "; scenario=" + scenario + "; mode=" + mode
                    + "; endpoint restored=true; existing framework pings=" + pings.get();
        } catch (Throwable error) {
            error.printStackTrace(System.out);
        } finally {
            running.set(false);
            if (observer != null && observer.isAlive()) {
                try { observer.join(5000); } catch (InterruptedException error) { Thread.currentThread().interrupt(); }
            }
            System.out.flush();
        }
    }
    @Override public void onResume() {
        super.onResume();
        System.out.println("BinderSgActivity onResume: " + verdict);
        System.out.flush();
        finish();
    }
}
