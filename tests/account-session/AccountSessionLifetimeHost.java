/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
import android.accounts.AccountManager;
import android.accounts.IAccountManagerResponse;
import android.content.ComponentName;
import android.content.Intent;
import android.os.Bundle;
import java.lang.reflect.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicLong;
import java.util.function.LongSupplier;

/** Actual production Answer, caller authentication and timer; synthetic authenticator metadata. */
public final class AccountSessionLifetimeHost {
    static int checks;
    static void check(boolean ok, String why) { if (!ok) throw new AssertionError(why); checks++; }
    static Class<?> type(String name) throws Exception { return Class.forName("com.gesher.server." + name); }
    static Field field(Class<?> c, String n) throws Exception { Field f=c.getDeclaredField(n); f.setAccessible(true); return f; }
    static Object get(Object o, String n) throws Exception { return field(o.getClass(), n).get(o); }
    static void set(Object o, String n, Object v) throws Exception { field(o.getClass(), n).set(o,v); }
    static Object call(Object o, String n, Class<?>[] p, Object... a) throws Exception {
        Method m=o.getClass().getDeclaredMethod(n,p); m.setAccessible(true);
        try { return m.invoke(o,a); } catch (InvocationTargetException e) {
            if (e.getCause() instanceof Error x) throw x;
            if (e.getCause() instanceof Exception x) throw x;
            throw e;
        }
    }
    static Object make(Class<?> c, Class<?>[] p, Object... a) throws Exception {
        Constructor<?> k=c.getDeclaredConstructor(p); k.setAccessible(true); return k.newInstance(a);
    }
    static class Response extends IAccountManagerResponse.Stub {
        volatile int results, errors, code;
        volatile String message;
        Runnable onLogin;
        final CountDownLatch error = new CountDownLatch(1);
        public void onResult(Bundle b) throws android.os.RemoteException { results++; if (b.containsKey(AccountManager.KEY_INTENT) && onLogin != null) onLogin.run(); }
        public void onError(int code, String text) { this.code=code; message=text; errors++; error.countDown(); }
    }
    static final class Harness implements AutoCloseable {
        final Class<?> service=type("AccountManagerService"), auth=type("AccountManagerService$Authenticator");
        final Class<?> answerClass=type("AccountManagerService$Answer"), checkClass=type("AccountManagerService$ResultCheck");
        final Object manager, owner, validator;
        Object answer;
        final Response response=new Response();
        final AtomicLong uptime=new AtomicLong(1000);
        boolean hasClock;
        ScheduledThreadPoolExecutor timer;
        Harness() throws Exception {
            Class<?> pm=type("PackageManagerService");
            Object packages=make(pm,new Class<?>[]{Collection.class},Collections.emptyList());
            manager=make(service,new Class<?>[]{type("ActivityManagerService"),pm,boolean.class},null,packages,false);
            owner=make(auth,new Class<?>[]{String.class,ComponentName.class,int.class,int.class,int.class,int.class,boolean.class},
                    "fixture.type",new ComponentName("fixture.authenticator","fixture.authenticator.Service"),0,0,0,0,false);
            try { set(manager,"mSessionUptimeMillis",(LongSupplier)uptime::get); hasClock=true; }
            catch (NoSuchFieldException baseline) { /* Retain the pre-repair retention negative control. */ }
            answer=newAnswer();
            Object raw=get(manager,"mSessionTimeout");
            if (!(raw instanceof ScheduledThreadPoolExecutor)) raw=get(raw,"e");
            timer=(ScheduledThreadPoolExecutor)raw;
            validator=Proxy.newProxyInstance(checkClass.getClassLoader(),new Class<?>[]{checkClass},
                    (p,m,a)->((Bundle)a[0]).getBoolean("fixture.reject")?null:a[0]);
        }
        Object newAnswer() throws Exception { return make(answerClass,new Class<?>[]{service,auth},manager,owner); }
        Object newAnswer(IAccountManagerResponse response) throws Exception {
            return make(answerClass,new Class<?>[]{service,auth,type("AccountManagerService$Caller"),IAccountManagerResponse.class},
                    manager,owner,null,response);
        }
        void expire() throws Exception {
            if (hasClock) uptime.set((Long)get(answer,"deadline"));
            Runnable expiry = (Runnable) timer.getQueue().peek();
            timer.remove(expiry);
            expiry.run();
        }
        void result(Bundle b) throws Exception { call(answer,"onResult",new Class<?>[]{Bundle.class},b); }
        void start() throws Exception {
            start(false);
        }
        void start(boolean queued) throws Exception {
            Bundle login=new Bundle(); login.putParcelable(AccountManager.KEY_INTENT,new Intent("fixture.login"));
            result(login);
            if (queued) { result(new Bundle()); result(new Bundle()); }
            call(answer,"deliverSession",new Class<?>[]{IAccountManagerResponse.class,checkClass},response,validator);
        }
        void released() throws Exception {
            check(timer.getQueue().isEmpty(),"terminal Answer retained a session timeout task");
            check(get(answer,"listener")==null && get(answer,"errorListener")==null && get(answer,"result")==null
                    && ((List<?>)get(answer,"pending")).isEmpty(),"terminal Answer retained its response/result graph");
        }
        public void close() throws Exception {
            timer.shutdownNow(); ((ExecutorService)get(manager,"mCalls")).shutdownNow();
            ((ExecutorService)get(get(manager,"mPm"),"mPackageEventExecutor")).shutdownNow();
        }
    }
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        for (String terminal:List.of("success","error","removal","timeout","reentrant","invalid","queued")) {
            try (Harness h=new Harness()) {
                if (terminal.equals("reentrant")) h.response.onLogin=()->{
                    try { h.result(new Bundle()); } catch (Exception e) { throw new AssertionError(e); }
                };
                h.start(terminal.equals("queued"));
                if (!terminal.equals("reentrant") && !terminal.equals("queued")) check(h.timer.getQueue().size()==1,"interactive session has no timeout");
                switch (terminal) {
                    case "success" -> { h.result(new Bundle()); h.result(new Bundle()); }
                    case "error" -> call(h.answer,"onError",new Class<?>[]{int.class,String.class},5,"fixture error");
                    case "removal" -> { call(h.answer,"cancelForRemoval",new Class<?>[0]); check(h.response.error.await(3,TimeUnit.SECONDS),"removal callback missing"); }
                    case "timeout" -> {
                        h.expire();
                        check(h.response.error.await(3,TimeUnit.SECONDS),"timeout callback missing");
                    }
                    case "invalid" -> { Bundle invalid=new Bundle(); invalid.putBoolean("fixture.reject",true); h.result(invalid); }
                }
                h.released();
                boolean success=terminal.equals("success")||terminal.equals("reentrant")||terminal.equals("queued");
                check(h.response.results==(success?2:1),"result callback count changed");
                check(h.response.errors==(success?0:1),"error callback count changed");
            }
        }
        try (Harness h=new Harness()) {
            CountDownLatch scheduling=new CountDownLatch(1), permit=new CountDownLatch(1), completing=new CountDownLatch(1);
            java.util.concurrent.atomic.AtomicReference<Throwable> failure=new java.util.concurrent.atomic.AtomicReference<>();
            call(h.answer,"close",new Class<?>[0]);
            Object previous=h.answer;
            h.timer.shutdownNow();
            h.timer=new ScheduledThreadPoolExecutor(1) {
                @Override public ScheduledFuture<?> schedule(Runnable task,long delay,TimeUnit unit) {
                    scheduling.countDown();
                    try { if (!permit.await(3,TimeUnit.SECONDS)) throw new AssertionError("schedule blocked"); }
                    catch (InterruptedException e) { throw new AssertionError(e); }
                    return super.schedule(task,delay,unit);
                }
            };
            h.timer.setRemoveOnCancelPolicy(true); set(h.manager,"mSessionTimeout",h.timer);
            Thread start=new Thread(()->{try {h.answer=h.newAnswer();}catch(Throwable e){failure.set(e);}});
            Thread finish=new Thread(()->{
                completing.countDown();
                try {
                    Object constructing;
                    synchronized (h.manager) {
                        constructing=((Set<?>)get(h.manager,"mAnswers")).stream().filter(a->a!=previous).findFirst().orElseThrow();
                    }
                    Bundle login=new Bundle(); login.putParcelable(AccountManager.KEY_INTENT,new Intent("fixture.login"));
                    call(constructing,"onResult",new Class<?>[]{Bundle.class},login);
                    call(constructing,"deliverSession",new Class<?>[]{IAccountManagerResponse.class,h.checkClass},h.response,h.validator);
                    call(constructing,"onResult",new Class<?>[]{Bundle.class},new Bundle());
                } catch(Throwable e) {failure.set(e);}
            });
            try {
                start.start(); check(scheduling.await(3,TimeUnit.SECONDS),"schedule gate not reached");
                finish.start(); check(completing.await(3,TimeUnit.SECONDS),"completion did not start");
            } finally { permit.countDown(); }
            start.join(5000); finish.join(5000);
            check(!start.isAlive()&&!finish.isAlive()&&failure.get()==null,"schedule/complete race failed: "+failure.get());
            h.released();
            check(h.response.results==2 && h.response.errors==0,"race changed result delivery");
        }
        System.out.println("AccountSessionLifetimeHost passed: "+checks+" checks");
    }
}
