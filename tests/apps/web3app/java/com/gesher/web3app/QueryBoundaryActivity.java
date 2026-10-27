package com.gesher.web3app;

import android.app.Activity;
import android.app.Dialog;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.view.View;
import android.widget.Button;
import android.widget.TextView;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.Map;
import java.util.concurrent.atomic.AtomicInteger;

/** Real mapped Dialog views: positive action, canceled queued action and stale generation. */
public final class QueryBoundaryActivity extends Activity {
    private static native long nativeStart(long hwnd, int kind, long node, boolean direct);
    private static native long nativeState(long id);
    private static native long nativeGeneration(long id);
    private static native boolean nativeJoin(long id);
    private static native int nativeActive();
    private static native int nativeAttached();
    private static native int nativeShutdowns();
    private static native boolean nativeStalePerform(long id);
    private final Handler handler = new Handler();
    private Class<?> bridge;
    private Dialog dialog;
    private Button button;
    private int clicks, cycle;
    private long previousGeneration;
    private void check(boolean okay, String text) {
        if (!okay) throw new AssertionError(text);
        Web3.log("query boundary PASS " + text);
    }
    private Object call(String name, Class<?>[] signature, Object... args) throws Exception {
        Method method = bridge.getDeclaredMethod(name, signature);
        method.setAccessible(true);
        return method.invoke(null, args);
    }
    private int pending() throws Exception {
        Field field = bridge.getDeclaredField("sPendingActions"); field.setAccessible(true);
        return ((AtomicInteger) field.get(null)).get();
    }
    private int cached() throws Exception {
        Field field = bridge.getDeclaredField("sRoots"); field.setAccessible(true);
        return ((Map<?, ?>) field.get(null)).size();
    }
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        if (!"true".equals(System.getenv("GITHUB_ACTIONS"))) throw new AssertionError("CI only");
        TextView label = new TextView(this); label.setText("Actual mapped query cancellation");
        setContentView(label);
        try {
            bridge = Class.forName("com.gesher.app.accessibility.UiaBridge");
            check(Boolean.TRUE.equals(call("start", new Class<?>[0])), "bridge starts for mapped views");
            check(Boolean.TRUE.equals(call("nativeRegisterFixture", new Class<?>[]{Class.class},
                    QueryBoundaryActivity.class)), "real mapped HWND hook registered");
        } catch (Exception e) { throw new AssertionError(e); }
        handler.postDelayed(this::newDialog, 200);
    }
    private void newDialog() {
        try {
            cycle++;
            clicks = 0;
            button = new Button(this); button.setText("Count real UIA actions");
            button.setOnClickListener(v -> clicks++);
            dialog = new Dialog(this); dialog.setContentView(button); dialog.show();
            handler.postDelayed(this::warm, 100);
        } catch (Exception e) { throw new AssertionError(e); }
    }
    private long start(int kind, long node) throws Exception {
        View decor = dialog.getWindow().getDecorView();
        long hwnd = (Long) call("hwndOf", new Class<?>[]{IBinder.class}, decor.getWindowToken());
        check(hwnd != 0 && decor.isAttachedToWindow(), "actual attached Dialog HWND cycle=" + cycle);
        long id = nativeStart(hwnd, kind, node, false);
        check(id != 0, "real mapped owner request kind=" + kind);
        return id;
    }
    private void await(long id, Runnable done) {
        long deadline = android.os.SystemClock.uptimeMillis() + 4000;
        handler.post(new Runnable() {
            @Override public void run() {
                long state = nativeState(id);
                if ((state & 2) != 0) {
                    check((state & 4) == 0, "positive real mapped query succeeds");
                    check(nativeJoin(id), "positive sender really exits0");
                    done.run();
                } else {
                    check(android.os.SystemClock.uptimeMillis() < deadline, "positive query retains original3s budget");
                    handler.postDelayed(this, 5);
                }
            }
        });
    }
    private void warm() {
        try {
            long id = start(0, -1);
            long generation = nativeGeneration(id);
            check(generation > previousGeneration, "mapped HWND lifetime advances cycle=" + cycle);
            previousGeneration = generation;
            await(id, this::positiveClick);
        } catch (Exception e) { throw new AssertionError(e); }
    }
    private void positiveClick() {
        try {
            long node = button.createAccessibilityNodeInfo().getSourceNodeId();
            long id = start(3, node);
            await(id, () -> {
                check(clicks == 1, "same real node action mutates live View exactly once");
                clicks = 0;
                handler.post(this::cancelQueuedClick);
            });
        } catch (Exception e) { throw new AssertionError(e); }
    }
    private void cancelQueuedClick() {
        try {
            int cacheBefore = cached();
            long began = System.nanoTime();
            long id = start(3, button.createAccessibilityNodeInfo().getSourceNodeId());
            // We ARE the actual UI thread. Keep it occupied until the existing
            // real native query times out, proving its posted action is removed.
            long admission = System.nanoTime() + 1_500_000_000L;
            while (pending() == 0 && System.nanoTime() < admission) Thread.sleep(2);
            check(pending() > 0, "actual ACTION Handler task queued after cached-root lookup while UI thread held");
            long bound = began + 3_500_000_000L;
            while ((nativeState(id) & 2) == 0 && System.nanoTime() < bound) Thread.sleep(2);
            check((nativeState(id) & 6) == 6, "original3s query returns unavailable before UI release");
            long clean = System.nanoTime() + 500_000_000L;
            while ((pending() != 0 || nativeActive() != 0 || nativeAttached() != 0)
                    && System.nanoTime() < clean) Thread.sleep(2);
            check(pending() == 0 && nativeActive() == 0 && nativeAttached() == 0,
                    "canceled posted Handler task removed and actual JNI detached");
            handler.postDelayed(() -> afterCancel(id, cacheBefore), 100);
        } catch (Exception e) { throw new AssertionError(e); }
    }
    private void afterCancel(long id, int cacheBefore) {
        try {
            check(clicks == 0 && cached() == cacheBefore, "UI release causes no late action or cache publication");
            dialog.dismiss(); // real WindowSession removal and real native owner join
            check(!nativeStalePerform(id), "retired real Dialog generation rejects stale action");
            check(clicks == 0, "retired generation cannot mutate its old View");
            check(nativeJoin(id), "canceled native sender really exits0");
            if (cycle < 2) handler.postDelayed(this::newDialog, 100);
            else {
                Web3.log("query boundary mapped done: positive=2 canceled=2 stale=2 attached=0");
                finish();
            }
        } catch (Exception e) { throw new AssertionError(e); }
    }
}
