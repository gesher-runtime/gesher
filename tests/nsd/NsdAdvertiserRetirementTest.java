/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.net.mdns;

import static com.gesher.net.mdns.NsdAdvertiserContractTest.*;
import com.gesher.net.WindowsNsdAdvertiser.*;
import android.net.nsd.NsdServiceInfo;
import java.io.IOException;
import java.lang.reflect.Field;
import java.net.InetSocketAddress;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/** Bounded scheduling/error seams only: no native socket, packet or Windows execution. */
final class NsdAdvertiserRetirementTest {
    private static Object field(Owner owner, String name) {
        try { Field f=Owner.class.getDeclaredField(name); f.setAccessible(true); return f.get(owner); }
        catch(ReflectiveOperationException e) { throw new AssertionError(e); }
    }
    private static boolean stopped(Owner owner) {
        synchronized(field(owner,"queueLock")) { return field(owner,"thread")==null; }
    }
    private static int reservations(Owner owner) {
        synchronized(field(owner,"queueLock")) { return ((Map<?,?>)field(owner,"admissions")).size(); }
    }
    private static final class EmptyDriver implements Driver {
        final CountDownLatch polled=new CountDownLatch(1), release=new CountDownLatch(1);
        final CountDownLatch firstClose=new CountDownLatch(1), closed=new CountDownLatch(1);
        final List<Long> closeTimes=new ArrayList<>();
        final boolean reconcileError;
        final int rejectedCloses;
        int closeCalls, retiringOperations;
        EmptyDriver(boolean error,int rejectedCloses) {
            this.reconcileError=error;this.rejectedCloses=rejectedCloses;
        }
        private void usable() throws IOException {
            if(closeCalls!=0) {retiringOperations++;throw new IOException("controlled retiring handle");}
        }
        public Scope[] interfaces()throws IOException {usable();return new Scope[0];}
        public boolean configure(Scope[] scopes)throws IOException {usable();return true;}
        public long generation()throws IOException {
            usable();if(reconcileError)throw new IOException("controlled reconciliation failure");return 1;
        }
        public Packet poll(int timeout)throws IOException {
            usable();polled.countDown();
            try {if(!release.await(5,TimeUnit.SECONDS))throw new IOException("fixture release deadline");}
            catch(InterruptedException e){Thread.currentThread().interrupt();throw new IOException(e);}
            return null;
        }
        public long send(Scope scope,int family,InetSocketAddress destination,byte[][] bytes) {
            throw new AssertionError("No scope exists in this ownership fixture");
        }
        public boolean close() {
            closeTimes.add(System.nanoTime());closeCalls++;firstClose.countDown();
            if(closeCalls<=rejectedCloses)return false;
            closed.countDown();return true;
        }
    }
    private static Callback callback(AtomicInteger failed) {
        return new Callback(){
            public void registered(NsdServiceInfo ignored){throw new AssertionError("No-scope success");}
            public void failed(int error,boolean retained){check(error==0&&!retained,"real backend error profile");failed.incrementAndGet();}
        };
    }
    static void run()throws Exception {
        EmptyDriver full=new EmptyDriver(false,1);AtomicInteger failures=new AtomicInteger();
        Owner owner=new Owner(()->full,()->TimeUnit.NANOSECONDS.toMillis(System.nanoTime()),()->0);
        owner.register(71,info("OwnedQueuedWithdrawal"),MdnsAdvertisingOptions.getDefaultOptions(),12001,callback(failures));
        check(full.polled.await(4,TimeUnit.SECONDS),"owned registration reached its normal no-scope poll");
        for(int i=0;i<4096;i++)owner.update(71,info("OwnedQueuedWithdrawal"),MdnsAdvertisingOptions.getDefaultOptions());
        try {owner.update(71,info("OwnedQueuedWithdrawal"),MdnsAdvertisingOptions.getDefaultOptions());
            throw new AssertionError("ordinary queue exceeded capacity");}
        catch(RejectedExecutionException expected){checks++;}
        CountDownLatch done=new CountDownLatch(1);AtomicInteger completions=new AtomicInteger();
        Runnable completion=()->{completions.incrementAndGet();done.countDown();};
        owner.unregister(71,completion);owner.unregister(71,completion);owner.withdraw(71);owner.withdraw(71);
        check(reservations(owner)==1,"full queue retains one coalesced withdrawal reservation");
        full.release.countDown();check(done.await(4,TimeUnit.SECONDS),"reserved withdrawal admitted despite full ordinary queue");
        check(full.closed.await(4,TimeUnit.SECONDS),"normal retirement eventually releases its real driver boundary");
        Owner completed=owner;await(()->stopped(completed),"owner worker exits after withdrawal and actual driver release");
        check(completions.get()==1&&reservations(owner)==0&&failures.get()==0,"coalesced stop lost/duplicated completion or ownership");
        check(full.closeTimes.size()==2&&full.closeTimes.get(1)-full.closeTimes.get(0)>=900_000_000L,
                "normal close rejection uses the bounded retry path");

        CountDownLatch opened=new CountDownLatch(1),allowOpen=new CountDownLatch(1),withdrawn=new CountDownLatch(1);
        EmptyDriver pending=new EmptyDriver(false,0);
        owner=new Owner(()->{opened.countDown();try{if(!allowOpen.await(4,TimeUnit.SECONDS))throw new IOException("fixture open deadline");}
            catch(InterruptedException e){throw new IOException(e);}return pending;},()->TimeUnit.NANOSECONDS.toMillis(System.nanoTime()),()->0);
        owner.register(72,info("OwnedPendingAdd"),MdnsAdvertisingOptions.getDefaultOptions(),12001,callback(failures));
        check(opened.await(4,TimeUnit.SECONDS),"queued add entered the controlled constructor");
        owner.unregister(72,withdrawn::countDown);allowOpen.countDown();
        check(withdrawn.await(4,TimeUnit.SECONDS)&&pending.closed.await(4,TimeUnit.SECONDS),
                "withdrawal follows its accepted add and releases its reservation");

        EmptyDriver error=new EmptyDriver(true,2),next=new EmptyDriver(false,0),awaken=new EmptyDriver(false,0);
        AtomicInteger opens=new AtomicInteger();List<EmptyDriver> drivers=List.of(error,next,awaken);
        owner=new Owner(()->drivers.get(opens.getAndIncrement()),()->TimeUnit.NANOSECONDS.toMillis(System.nanoTime()),()->0);
        owner.register(73,info("OwnedErrorRetirement"),MdnsAdvertisingOptions.getDefaultOptions(),12001,callback(failures));
        check(error.firstClose.await(4,TimeUnit.SECONDS),"reconciliation failure reaches actual close boundary");
        owner.register(74,info("OwnedAfterRetirement"),MdnsAdvertisingOptions.getDefaultOptions(),12001,callback(failures));
        check(opens.get()==1,"new accepted work cannot reuse a retiring driver");
        check(error.closed.await(4,TimeUnit.SECONDS)&&next.polled.await(4,TimeUnit.SECONDS),
                "queued work resumes with a fresh driver after release");
        check(error.closeTimes.size()==3&&error.retiringOperations==0&&failures.get()==1,
                "error retirement reread a retiring handle or duplicated failure");
        for(int i=1;i<error.closeTimes.size();i++)check(error.closeTimes.get(i)-error.closeTimes.get(i-1)>=900_000_000L,
                "error close rejection retries at a bounded interval");
        System.out.println("RETIREMENT_CLOSE_GAPS_NS "+(error.closeTimes.get(1)-error.closeTimes.get(0))+" "+(error.closeTimes.get(2)-error.closeTimes.get(1)));
        owner.withdraw(74);next.release.countDown();check(next.closed.await(4,TimeUnit.SECONDS),"fresh driver is independently withdrawn");
        Owner retired=owner;await(()->stopped(retired),"worker stopped before later admission");
        owner.register(76,info("OwnedAfterWorkerExit"),MdnsAdvertisingOptions.getDefaultOptions(),12001,callback(failures));
        check(awaken.polled.await(4,TimeUnit.SECONDS)&&opens.get()==3,
                "fresh accepted ownership wakes a stopped worker with a new driver");
        owner.withdraw(76);awaken.release.countDown();check(awaken.closed.await(4,TimeUnit.SECONDS),"woken owner retires independently");
        CountDownLatch failure=new CountDownLatch(1);Owner construction=new Owner(()->{throw new IOException("controlled constructor");},()->TimeUnit.NANOSECONDS.toMillis(System.nanoTime()),()->0);
        construction.register(75,info("OwnedOpenFailure"),MdnsAdvertisingOptions.getDefaultOptions(),12001,new Callback(){
            public void registered(NsdServiceInfo ignored){throw new AssertionError("constructor success");}
            public void failed(int code,boolean retained){failure.countDown();}
        });
        check(failure.await(4,TimeUnit.SECONDS),"constructor error observed");
        await(()->stopped(construction),"failed constructor worker retires");
        check(reservations(construction)==0,"failed add/constructor retained its reserved ownership slot");
    }
}
