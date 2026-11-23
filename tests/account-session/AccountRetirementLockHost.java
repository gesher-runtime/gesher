/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
import android.accounts.AccountManager;
import android.content.Intent;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedActivityImpl;
import com.android.internal.pm.pkg.component.ParsedIntentInfoImpl;
import java.lang.management.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

/** Two ordinary operations, paused at declared scheduling boundaries.
 * Actual production notification drain, recipient lifetime, PM query and account
 * query are unchanged. No workload pressure or native execution is involved.
 */
public final class AccountRetirementLockHost {
  public static void main(String[] args) throws Exception {
    System.load(args[0]); android.app.PropertyInvalidatedCache.setTestMode(true);
    boolean manifest=Boolean.parseBoolean(args[1]);
    var h=new AccountRetirementHost.H();
    if(manifest){
      var receiver=new ParsedActivityImpl();
      receiver.setName(AccountRetirementHost.CLIENT+".Receiver");receiver.setPackageName(AccountRetirementHost.CLIENT);
      receiver.setProcessName(AccountRetirementHost.CLIENT);receiver.setEnabled(true);receiver.setExported(true);
      var filter=new ParsedIntentInfoImpl();filter.getIntentFilter().addAction(AccountManager.LOGIN_ACCOUNTS_CHANGED_ACTION);
      receiver.addIntent(filter);
      var parsed=(PackageImpl)PackageImpl.forTesting(AccountRetirementHost.CLIENT,"/fixture/"+AccountRetirementHost.CLIENT+"/base.apk");
      parsed.setEnabled(true);parsed.setTargetSdkVersion(36);parsed.addReceiver(receiver);parsed.hideAsFinal();
      AccountRetirementHost.set(h.client,"parsed",parsed);
    }
    AccountRetirementHost.set(h.accounts,"mAm",h.am);
    Object caller=AccountRetirementHost.call(h.accounts,"captureCaller",
      new Class<?>[]{String.class,String.class,boolean.class,int.class,int.class},AccountRetirementHost.OTHER,
      AccountRetirementHost.call(h.accounts,"packageIdentity",new Class<?>[]{String.class},AccountRetirementHost.OTHER),false,12003,873502);
    h.retireOwner();
    CountDownLatch beforeManifest=new CountDownLatch(1),accountsHeld=new CountDownLatch(1),queryMayEnter=new CountDownLatch(1);
    AtomicReference<Throwable> notificationFailure=new AtomicReference<>(),queryFailure=new AtomicReference<>();
    AccountRetirementHost.set(h.bc,"mBeforeSenderAuthentication",(Runnable)()->{
      try { AccountRetirementHost.set(h.bc,"mBeforeSenderAuthentication",null);beforeManifest.countDown();
        if(!accountsHeld.await(3,TimeUnit.SECONDS))throw new AssertionError("account scheduling boundary timed out");
      }catch(Exception e){throw new AssertionError(e);}
    });
    Thread query=new Thread(()->{
      try{
        if(!beforeManifest.await(3,TimeUnit.SECONDS))throw new AssertionError("notification scheduling boundary timed out");
        // accountsByTypeForClient is itself synchronized on precisely this monitor.
        // Hold it at entry so the other ordinary call reaches its opposite ordering.
        synchronized(h.accounts){
          accountsHeld.countDown();
          if(!queryMayEnter.await(3,TimeUnit.SECONDS))throw new AssertionError("query scheduling boundary timed out");
          AccountRetirementHost.call(h.accounts,"accountsByTypeForClient",
            new Class<?>[]{String.class,String.class,caller.getClass()},"fixture.accounts.other",AccountRetirementHost.CLIENT,caller);
        }
      }catch(Throwable e){queryFailure.set(e);}
    },"ordinary-account-query");query.setDaemon(true);
    Thread notification=new Thread(()->{try{h.queue.drain();}catch(Throwable e){notificationFailure.set(e);}},"ordinary-removal-notification");notification.setDaemon(true);
    query.start();notification.start();
    if(!accountsHeld.await(3,TimeUnit.SECONDS))throw new AssertionError("account monitor never entered");
    ThreadMXBean bean=ManagementFactory.getThreadMXBean();
    long deadline=System.nanoTime()+TimeUnit.SECONDS.toNanos(3);ThreadInfo atBoundary=null;
    while(System.nanoTime()<deadline){
      ThreadInfo info=bean.getThreadInfo(new long[]{notification.threadId()},true,true)[0];
      if(info!=null&&info.getThreadState()==Thread.State.BLOCKED&&info.getLockOwnerId()==query.threadId()){atBoundary=info;break;}
      if (!notification.isAlive()) break;
      Thread.sleep(5);
    }
    if(atBoundary==null && notification.isAlive())throw new AssertionError("notification neither completed nor reached the scheduling boundary: "+notificationFailure.get());
    boolean holdsPm=atBoundary!=null && Arrays.stream(atBoundary.getLockedMonitors()).anyMatch(m->m.getIdentityHashCode()==System.identityHashCode(h.pm));
    System.out.println("manifest="+manifest+" notification-waits-account="+(atBoundary!=null)+" notification-holds-pm="+holdsPm);
    queryMayEnter.countDown();
    deadline=System.nanoTime()+TimeUnit.SECONDS.toNanos(2);long[] cycle=null;
    while(System.nanoTime()<deadline){cycle=bean.findDeadlockedThreads();if(cycle!=null||!query.isAlive()&&!notification.isAlive())break;Thread.sleep(5);}
    if(cycle!=null){
      for(ThreadInfo info:bean.getThreadInfo(cycle,true,true))System.out.println(info);
      System.out.println("CONFIRMED: package monitor -> account monitor -> package monitor");System.exit(17);
    }
    query.join(1000);notification.join(1000);
    if(query.isAlive()||notification.isAlive())throw new AssertionError("thread remained live without a detected monitor cycle");
    if(notificationFailure.get()!=null)throw new AssertionError(notificationFailure.get());
    System.out.println("PASS: both ordinary calls terminate; query outcome="+queryFailure.get());h.close();
  }
}
