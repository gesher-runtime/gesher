/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
import android.accounts.AccountManager;
import android.accounts.IAccountManagerResponse;
import android.content.Intent;
import android.os.Bundle;
import android.os.IBinder;
import android.os.RemoteException;
import java.lang.reflect.Proxy;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;
import java.util.function.BooleanSupplier;

/**
 * Whole production Answer and its chooser wait helper. Only uptime, authenticator
 * metadata and response Binder death are explicit host boundaries; scheduled tasks,
 * state transitions, caller authentication and callback delivery are production code.
 * No binding to an authenticator APK, credentials, login UI or remote Binder is used.
 */
public final class AccountSessionTimeoutHost {
    static int checks;
    static void check(boolean ok, String why) { if (!ok) throw new AssertionError(why); checks++; }
    static Object call(Object o, String n, Class<?>[] p, Object... a) throws Exception {
        return AccountSessionLifetimeHost.call(o,n,p,a);
    }
    static Object get(Object o, String n) throws Exception { return AccountSessionLifetimeHost.get(o,n); }
    static Bundle login() {
        Bundle b=new Bundle(); b.putParcelable(AccountManager.KEY_INTENT,new Intent("fixture.login")); return b;
    }
    static long deadline(AccountSessionLifetimeHost.Harness h) throws Exception { return (Long)get(h.answer,"deadline"); }
    static Runnable task(AccountSessionLifetimeHost.Harness h) { return (Runnable)h.timer.getQueue().peek(); }
    static void register(AccountSessionLifetimeHost.Harness h, IAccountManagerResponse r) throws Exception {
        call(h.answer,"deliverSession",new Class<?>[]{IAccountManagerResponse.class,h.checkClass},r,h.validator);
    }
    static void replace(AccountSessionLifetimeHost.Harness h, IAccountManagerResponse r) throws Exception {
        call(h.answer,"close",new Class<?>[0]); h.answer=h.newAnswer(r);
    }
    static Thread start(Runnable r) { Thread t=new Thread(r); t.setDaemon(true); t.start(); return t; }
    static void join(Thread t) throws Exception { t.join(3000); check(!t.isAlive(),"worker did not finish"); }
    static void retired(AccountSessionLifetimeHost.Harness h) throws Exception {
        h.released();
        check(get(h.answer,"responseBinder")==null,"closed Answer retains a response death link");
        check(((CountDownLatch)get(h.answer,"done")).getCount()==0,"terminal Answer did not release its initial waiter");
    }
    static final class TrackedResponse extends AccountSessionLifetimeHost.Response {
        IBinder.DeathRecipient recipient;
        int links, unlinks;
        boolean dead, rejectResult;
        final IBinder binder=(IBinder)Proxy.newProxyInstance(IBinder.class.getClassLoader(),new Class<?>[]{IBinder.class},(p,m,a)->{
            switch (m.getName()) {
                case "linkToDeath":
                    if (dead) throw new RemoteException("fixture already dead");
                    recipient=(IBinder.DeathRecipient)a[0]; links++; return null;
                case "unlinkToDeath":
                    boolean present=recipient==a[0]; recipient=null; unlinks++; return present;
                case "isBinderAlive": case "pingBinder": return !dead;
                case "hashCode": return System.identityHashCode(p);
                case "equals": return p==a[0];
                case "toString": return "fixture response Binder";
                default: throw new AssertionError("unexpected Binder method "+m.getName());
            }
        });
        @Override public IBinder asBinder() { return binder; }
        @Override public void onResult(Bundle b) throws RemoteException {
            if (rejectResult) throw new RemoteException("fixture caller gone");
            super.onResult(b);
        }
        void die() { dead=true; IBinder.DeathRecipient r=recipient; if(r!=null) r.binderDied(); }
    }

    static void budgetAndInitial() throws Exception {
        try (var h=new AccountSessionLifetimeHost.Harness()) {
            check(h.timer.getQueue().size()==1,"initial binding has no construction-time timeout");
            check(h.hasClock,"the session clock boundary is unavailable");
            long end=deadline(h);
            check(end-h.uptime.get()==TimeUnit.MINUTES.toMillis(15),"session budget is not 15 uptime minutes");
            Runnable original=task(h);
            long delay=((Delayed)original).getDelay(TimeUnit.MILLISECONDS);
            check(delay>899000 && delay<=900000,"construction-time task has the wrong delay");
            h.uptime.addAndGet(TimeUnit.MINUTES.toMillis(2));
            h.start();
            h.result(login());
            call(h.answer,"onRequestContinued",new Class<?>[0]);
            check(task(h)==original && deadline(h)==end,"initial/continued responses restarted the timer");
            original.run(); // A host wakeup is not necessarily an Android uptime expiry.
            check(h.response.errors==0 && h.timer.getQueue().size()==1,"early wakeup expired/duplicated the session");
            delay=((Delayed)task(h)).getDelay(TimeUnit.MILLISECONDS);
            check(delay>779000 && delay<=780000,"early wakeup did not preserve the remaining original budget");
            h.uptime.set(end);
            task(h).run();
            h.result(new Bundle());
            check(h.response.results==2 && h.response.errors==1 && h.response.code==AccountManager.ERROR_CODE_REMOTE_EXCEPTION,
                    "continued-session timeout did not report exactly one remote failure");
            retired(h);
        }
        try (var h=new AccountSessionLifetimeHost.Harness()) {
            FutureTask<Void> initial=new FutureTask<>(()->{call(h.answer,"awaitInitial",new Class<?>[]{BooleanSupplier.class},(Object)null); return null;});
            start(initial);
            h.expire();
            initial.get(3,TimeUnit.SECONDS);
            register(h,h.response); register(h,h.response);
            call(h.answer,"onError",new Class<?>[]{int.class,String.class},AccountManager.ERROR_CODE_BAD_ARGUMENTS,"late error");
            h.result(login());
            check(h.response.results==0 && h.response.errors==1 && h.response.code==AccountManager.ERROR_CODE_REMOTE_EXCEPTION,
                    "timeout before registration was lost, overwritten or replayed");
            retired(h);
        }
        try (var h=new AccountSessionLifetimeHost.Harness()) {
            TrackedResponse r=new TrackedResponse(); replace(h,r);
            check(r.links==1,"initial response was not watched before binding");
            h.expire();
            check(r.errors==1 && r.code==AccountManager.ERROR_CODE_REMOTE_EXCEPTION,"initial timeout did not reach the known response immediately");
            register(h,r); h.result(new Bundle());
            check(r.errors==1 && r.results==0 && r.unlinks==1,"initial timeout duplicated delivery or retained death registration");
            retired(h);
        }
        for (boolean removal:new boolean[]{false,true}) {
            try (var h=new AccountSessionLifetimeHost.Harness()) {
                if(removal) call(h.answer,"cancelForRemoval",new Class<?>[0]);
                else call(h.answer,"onError",new Class<?>[]{int.class,String.class},AccountManager.ERROR_CODE_BAD_ARGUMENTS,"explicit authenticator error");
                register(h,h.response); register(h,h.response); h.result(new Bundle());
                check(h.response.errors==1 && h.response.results==0 && h.response.code==(removal?AccountManager.ERROR_CODE_CANCELED:AccountManager.ERROR_CODE_BAD_ARGUMENTS),
                        "early cancellation/authenticator error was replaced by timeout semantics");
                retired(h);
            }
        }
        try (var h=new AccountSessionLifetimeHost.Harness()) {
            Bundle value=new Bundle(); value.putBoolean(AccountManager.KEY_BOOLEAN_RESULT,true); h.result(value);
            Bundle taken=(Bundle)call(h.answer,"takeResult",new Class<?>[0]);
            check(taken!=null && taken.getBoolean(AccountManager.KEY_BOOLEAN_RESULT),"noninteractive result was cleared before consumption");
            check(call(h.answer,"takeResult",new Class<?>[0])==null,"noninteractive result was replayed");
            h.result(login()); retired(h);
        }
        try (var h=new AccountSessionLifetimeHost.Harness()) {
            call(h.answer,"close",new Class<?>[0]);
            Class<?> operation=AccountSessionLifetimeHost.type("AccountManagerService$Call");
            Object op=Proxy.newProxyInstance(operation.getClassLoader(),new Class<?>[]{operation},(p,m,a)->{throw new AssertionError("unexpected authenticator call");});
            h.answer=call(h.manager,"call",new Class<?>[]{h.auth,operation,AccountSessionLifetimeHost.type("AccountManagerService$Caller"),IAccountManagerResponse.class},h.owner,op,null,h.response);
            register(h,h.response);
            check(h.response.errors==1 && h.response.code==AccountManager.ERROR_CODE_REMOTE_EXCEPTION,"initial bind failure was lost or replayed");
            retired(h);
        }
    }

    static void callerLifetime() throws Exception {
        for (boolean continued:new boolean[]{false,true}) {
            try (var h=new AccountSessionLifetimeHost.Harness()) {
                TrackedResponse r=new TrackedResponse(); replace(h,r);
                if(continued) {h.result(login()); register(h,r);}
                r.die(); register(h,r); h.result(new Bundle());
                check(r.links==1 && r.unlinks==1 && r.recipient==null,"response death registration was not released exactly once");
                check(r.errors==0 && r.results==(continued?1:0),"a dead response received a late callback");
                retired(h);
            }
        }
        try (var h=new AccountSessionLifetimeHost.Harness()) {
            TrackedResponse r=new TrackedResponse(); r.dead=true; replace(h,r);
            register(h,r); h.result(login());
            check(r.errors==0 && r.results==0 && r.links==0 && r.unlinks==0,"already-dead response was treated as a live session");
            retired(h);
        }
        try (var h=new AccountSessionLifetimeHost.Harness()) {
            TrackedResponse r=new TrackedResponse(); r.rejectResult=true; replace(h,r);
            h.result(login()); register(h,r);
            check(r.results==0 && r.errors==0 && r.unlinks==1,"failed response IPC retained a live login session");
            retired(h);
        }
        try (var h=new AccountSessionLifetimeHost.Harness()) {
            call(h.answer,"awaitInitial",new Class<?>[]{BooleanSupplier.class},(BooleanSupplier)()->false);
            register(h,h.response); h.result(login());
            check(h.response.results==0 && h.response.errors==0,"abandoned initial chooser still delivered a response");
            retired(h);
        }
    }

    static void bindingCallbacks() throws Exception {
        for(String mode:List.of("null","dead","ipc","late","success")) {
            try(var h=new AccountSessionLifetimeHost.Harness()) {
                AtomicInteger invocations=new AtomicInteger();
                Class<?> operation=AccountSessionLifetimeHost.type("AccountManagerService$Call");
                Object op=Proxy.newProxyInstance(operation.getClassLoader(),new Class<?>[]{operation},(p,m,a)->{
                    invocations.incrementAndGet();
                    if(mode.equals("ipc")) throw new RemoteException("fixture authenticator IPC failure");
                    ((android.accounts.IAccountAuthenticatorResponse)a[1]).onResult(new Bundle());
                    return null;
                });
                android.app.IServiceConnection connection=(android.app.IServiceConnection)call(h.manager,"connection",
                        new Class<?>[]{h.answerClass,operation},h.answer,op);
                android.os.Binder binder=new android.os.Binder();
                android.accounts.IAccountAuthenticator authenticator=(android.accounts.IAccountAuthenticator)Proxy.newProxyInstance(
                        android.accounts.IAccountAuthenticator.class.getClassLoader(),new Class<?>[]{android.accounts.IAccountAuthenticator.class},
                        (p,m,a)->{if(m.getName().equals("asBinder"))return binder;throw new AssertionError("unexpected authenticator API");});
                binder.attachInterface(authenticator,"android.accounts.IAccountAuthenticator");
                if(mode.equals("late")) h.expire();
                connection.connected(null,mode.equals("null")?null:binder,null,mode.equals("dead"));
                register(h,h.response);
                check(invocations.get()==(mode.equals("success")||mode.equals("ipc")?1:0),"retired/null/dead binding invoked authenticator work");
                check(h.response.results==(mode.equals("success")?1:0) && h.response.errors==(mode.equals("success")?0:1),
                        "connection callback changed terminal delivery: "+mode);
                if(!mode.equals("success"))check(h.response.code==AccountManager.ERROR_CODE_REMOTE_EXCEPTION,"connection failure was not a remote error");
                int before=invocations.get();
                connection.connected(null,binder,null,false);
                check(invocations.get()==before,"late binding revived a completed session");
                retired(h);
            }
        }
    }

    static void chooserWaits() throws Exception {
        for(String terminal:List.of("timeout","success","cancel","interrupt")) {
            try (var h=new AccountSessionLifetimeHost.Harness()) {
                CountDownLatch finished=new CountDownLatch(1), presented=new CountDownLatch(1);
                AtomicBoolean success=new AtomicBoolean(), active=new AtomicBoolean(true), interrupted=new AtomicBoolean();
                AccountSessionLifetimeHost.Response response=new AccountSessionLifetimeHost.Response(){
                    @Override public void onResult(Bundle b) throws RemoteException {super.onResult(b); presented.countDown();}
                    @Override public void onError(int c,String text) {super.onError(c,text); finished.countDown();}
                };
                h.result(login());
                FutureTask<Boolean> wait=new FutureTask<>(()->{
                    boolean value=(Boolean)call(h.answer,"deliverToChooser",new Class<?>[]{IAccountManagerResponse.class,h.checkClass,CountDownLatch.class,BooleanSupplier.class,BooleanSupplier.class},
                            response,h.validator,finished,(BooleanSupplier)success::get,(BooleanSupplier)active::get);
                    interrupted.set(Thread.currentThread().isInterrupted()); return value;
                });
                Thread worker=start(wait);
                check(presented.await(3,TimeUnit.SECONDS),"chooser did not receive the initial intent");
                // More than the old independent ten-minute chooser limit has passed.
                h.uptime.addAndGet(TimeUnit.MINUTES.toMillis(11)); task(h).run();
                check(!wait.isDone() && response.errors==0,"chooser prematurely exhausted a separate budget");
                switch(terminal) {
                    case "timeout" -> h.expire();
                    case "success" -> {success.set(true); finished.countDown();}
                    case "cancel" -> active.set(false);
                    case "interrupt" -> worker.interrupt();
                }
                check(wait.get(3,TimeUnit.SECONDS)==terminal.equals("success"),"chooser terminal outcome changed: "+terminal);
                check(interrupted.get()==terminal.equals("interrupt"),"chooser lost/introduced interrupt status");
                check(response.results==1 && response.errors==(terminal.equals("timeout")?1:0),"chooser callback count changed");
                if(terminal.equals("timeout")) check(response.code==AccountManager.ERROR_CODE_REMOTE_EXCEPTION,"chooser changed timeout to cancellation");
                h.result(new Bundle()); register(h,response);
                check(response.results==1,"closed chooser accepted a late result");
                retired(h);
            }
        }
    }

    static void races() throws Exception {
        for(boolean terminal:new boolean[]{false,true}) {
            try(var h=new AccountSessionLifetimeHost.Harness()) {
                CountDownLatch validating=new CountDownLatch(1), permit=new CountDownLatch(1), expiring=new CountDownLatch(1);
                List<String> events=Collections.synchronizedList(new ArrayList<>());
                AtomicReference<Throwable> failed=new AtomicReference<>();
                AccountSessionLifetimeHost.Response response=new AccountSessionLifetimeHost.Response(){
                    @Override public void onResult(Bundle b)throws RemoteException {super.onResult(b); events.add(b.containsKey(AccountManager.KEY_INTENT)?"intent":"success");}
                    @Override public void onError(int c,String text) {super.onError(c,text); events.add("error:"+c);}
                };
                Object check=Proxy.newProxyInstance(h.checkClass.getClassLoader(),new Class<?>[]{h.checkClass},(p,m,a)->{
                    Bundle b=(Bundle)a[0];
                    if(b.getBoolean("fixture.block")){validating.countDown(); if(!permit.await(3,TimeUnit.SECONDS))throw new AssertionError("validation gate timed out");}
                    return b;
                });
                h.result(login()); call(h.answer,"deliverSession",new Class<?>[]{IAccountManagerResponse.class,h.checkClass},response,check);
                Bundle value=terminal?new Bundle():login(); value.putBoolean("fixture.block",true);
                Thread result=start(()->{try{h.result(value);}catch(Throwable e){failed.set(e);}});
                check(validating.await(3,TimeUnit.SECONDS),"validation gate was not reached");
                h.uptime.set(deadline(h)); Runnable expiry=task(h);
                Thread timer=start(()->{expiring.countDown();try{expiry.run();}catch(Throwable e){failed.set(e);}});
                try{check(expiring.await(3,TimeUnit.SECONDS),"timer race did not start");}finally{permit.countDown();}
                join(result); join(timer);
                check(failed.get()==null,"validation/expiry race failed: "+failed.get());
                check(events.equals(terminal?List.of("intent","success"):List.of("intent","intent","error:1")),"timeout overtook accepted delivery: "+events);
                retired(h);
            }
        }
        for(int iteration=0;iteration<16;iteration++) {
            try(var h=new AccountSessionLifetimeHost.Harness()) {
                h.start(); h.uptime.set(deadline(h)); Runnable expiry=task(h);
                CountDownLatch start=new CountDownLatch(1); AtomicReference<Throwable> failed=new AtomicReference<>();
                Thread error=start(()->{try{start.await();call(h.answer,"onError",new Class<?>[]{int.class,String.class},AccountManager.ERROR_CODE_BAD_ARGUMENTS,"authenticator error");}catch(Throwable e){failed.set(e);}});
                Thread timeout=start(()->{try{start.await();expiry.run();}catch(Throwable e){failed.set(e);}});
                start.countDown(); join(error); join(timeout);
                check(failed.get()==null && h.response.results==1 && h.response.errors==1,"racing terminal errors were lost/duplicated");
                check(h.response.code==AccountManager.ERROR_CODE_BAD_ARGUMENTS || h.response.code==AccountManager.ERROR_CODE_REMOTE_EXCEPTION,"racing error was replaced by cancellation");
                retired(h);
            }
        }
    }
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        budgetAndInitial(); callerLifetime(); bindingCallbacks(); chooserWaits(); races();
        System.out.println("AccountSessionTimeoutHost passed: "+checks+" checks");
    }
}
