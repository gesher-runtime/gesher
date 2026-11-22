/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
import android.accounts.Account;
import android.accounts.AccountManager;
import android.app.IApplicationThread;
import android.app.PropertyInvalidatedCache;
import android.content.Context;
import android.content.IIntentReceiver;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.Signature;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import java.io.IOException;
import java.lang.reflect.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.function.BiConsumer;
import sun.misc.Unsafe;

/** Independent ordinary lifecycle controls. Whole production services stay unchanged.
 * Boundaries: constructed installed-package metadata, local JNI identity/allocation,
 * protected-store writer, and a controllable executor for notification ordering.
 */
public final class AccountRetirementHost {
    static final Unsafe U;
    static int checks;
    static { try { Field f=Unsafe.class.getDeclaredField("theUnsafe"); f.setAccessible(true); U=(Unsafe)f.get(null); }
      catch(Exception e) {throw new ExceptionInInitializerError(e);} }
    static Class<?> t(String s)throws Exception{return Class.forName("com.gesher.server."+s);}
    static Field f(Class<?> c,String n)throws Exception{Field f=c.getDeclaredField(n);f.setAccessible(true);return f;}
    static Object get(Object o,String n)throws Exception{return f(o.getClass(),n).get(o);}
    static void set(Object o,String n,Object v)throws Exception{f(o.getClass(),n).set(o,v);}
    static Object make(Class<?> c,Class<?>[] p,Object...a)throws Exception{Constructor<?> k=c.getDeclaredConstructor(p);k.setAccessible(true);return k.newInstance(a);}
    static Object call(Object o,String n,Class<?>[] p,Object...a)throws Exception{
      Method m=o.getClass().getDeclaredMethod(n,p);m.setAccessible(true);
      try{return m.invoke(o,a);}catch(InvocationTargetException e){if(e.getCause() instanceof Error z)throw z;if(e.getCause() instanceof Exception z)throw z;throw e;}
    }
    static void check(boolean v,String why){if(!v)throw new AssertionError(why);checks++;}
    static final class Queue extends AbstractExecutorService {
      final List<Runnable> tasks=new ArrayList<>(); boolean stopped;
      public void execute(Runnable r){tasks.add(r);} public void shutdown(){stopped=true;}
      public List<Runnable> shutdownNow(){shutdown();return List.of();} public boolean isShutdown(){return stopped;}
      public boolean isTerminated(){return stopped;} public boolean awaitTermination(long n,TimeUnit u){return stopped;}
      void drain(){while(!tasks.isEmpty())tasks.remove(0).run();}
    }
    static Object pkg(String name,int uid)throws Exception{
      PackageImpl p=(PackageImpl)PackageImpl.forTesting(name,"/fixture/"+name+"/base.apk");
      p.setEnabled(true);p.setTargetSdkVersion(36);p.hideAsFinal();
      Object a=U.allocateInstance(t("ApkPackage")); set(a,"parsed",p);set(a,"packageName",name);set(a,"mUid",uid);
      set(a,"apkPath","/fixture/"+name+"/base.apk");set(a,"mDataDir","/fixture/"+name+"/data");
      set(a,"sharedLibraries",f(t("SharedLibraryGraph"),"EMPTY").get(null));
      set(a,"mSigningIdentityChecked",true);set(a,"mVerifiedSignatures",new Signature[]{new Signature(new byte[]{7})});return a;
    }
    static final String OWNER="fixture.owner",CLIENT="fixture.client",OTHER="fixture.other";
    static final Account REMOVED=new Account("one","fixture.accounts"),RETAINED=new Account("two","fixture.accounts.other");
    static final class AppThread extends IApplicationThread.Default {
      final Binder token=new Binder();final List<Intent> events=new ArrayList<>();
      public IBinder asBinder(){return token;}
      @Override public void scheduleRegisteredReceiver(IIntentReceiver r,Intent i,int c,String d,Bundle b,
          boolean o,boolean s,boolean a,int u,int ps,int uid,String p){events.add(new Intent(i));}
    }
    static final class H implements AutoCloseable {
      final Object owner=pkg(OWNER,12001), client=pkg(CLIENT,12002),other=pkg(OTHER,12003);
      final Object pm,atm,am,bc,accounts;
      final Queue queue=new Queue();Properties durable;boolean fail;
      final Map<Account,Object> entries;
      final AppThread thread=new AppThread();Object process;
      H()throws Exception {
        pm=make(t("PackageManagerService"),new Class<?>[]{Collection.class,boolean.class},List.of(owner,client,other),false);
        atm=make(t("ActivityTaskManagerService"),new Class<?>[]{t("PackageManagerService")},pm);
        am=U.allocateInstance(t("ActivityManagerService"));set(am,"mPm",pm);set(am,"mAtm",atm);
        set(am,"mCallers",make(t("CallerIdentity"),new Class<?>[]{t("PackageManagerService"),t("ActivityTaskManagerService")},pm,atm));
        bc=make(t("BroadcastController"),new Class<?>[]{t("ActivityTaskManagerService"),t("PackageManagerService")},atm,pm);set(am,"mBroadcasts",bc);
        Object policy=call(pm,"permissions",new Class<?>[0]);Object catalog=get(policy,"mCatalog");set(catalog,"mLoaded",true);set(catalog,"mFrameworkRead",true);
        accounts=make(t("AccountManagerService"),new Class<?>[]{t("ActivityManagerService"),t("PackageManagerService"),boolean.class},null,pm,false);
        ((ExecutorService)get(accounts,"mCalls")).shutdownNow();set(accounts,"mCalls",queue);
        entries=(Map<Account,Object>)get(accounts,"mAccounts");entries.clear();
        Class<?> writer=t("AccountManagerService$StoreWriter");set(accounts,"mStoreWriter",Proxy.newProxyInstance(writer.getClassLoader(),new Class<?>[]{writer},(p,m,a)->{
          if(fail)throw new IOException("fixture denied protected write");durable=new Properties();durable.putAll((Properties)a[0]);return null;
        }));
        entry(REMOVED,OWNER);entry(RETAINED,OTHER);save();
      }
      Object entry(Account a,String owner)throws Exception{
        Object e=make(t("AccountManagerService$Entry"),new Class<?>[0]);set(e,"owner",owner);
        set(e,"ownerIdentity",call(accounts,"packageIdentity",new Class<?>[]{String.class},owner));set(e,"password","fixture-secret");
        ((Map<String,Integer>)get(e,"visibility")).put(CLIENT+"\0"+call(accounts,"packageIdentity",new Class<?>[]{String.class},CLIENT),AccountManager.VISIBILITY_VISIBLE);
        ((Map<String,String>)get(e,"tokens")).put("scope","fixture-token");entries.put(a,e);return e;
      }
      void save()throws Exception{call(accounts,"beginMutation",new Class<?>[0]);call(accounts,"save",new Class<?>[0]);}
      boolean access(Account a)throws Exception{return (boolean)call(accounts,"canAccessAccount",new Class<?>[]{Account.class,String.class},a,CLIENT);}
      void begin()throws Exception{call(pm,"beginPackageRemoval",new Class<?>[]{t("ApkPackage")},owner);call(accounts,"packageRemoved",new Class<?>[]{String.class},OWNER);}
      void end()throws Exception{call(pm,"endPackageRemoval",new Class<?>[]{t("ApkPackage")},owner);}
      void reconcile(boolean installed)throws Exception{call(pm,"reconcileAccountRemovals",new Class<?>[]{Set.class},installed?Set.of(OWNER,CLIENT,OTHER):Set.of(CLIENT,OTHER));}
      void connectReceiver()throws Exception {
        set(accounts,"mAm",am);
        process=make(t("ActivityTaskManagerService$ProcessRecord"),new Class<?>[]{t("ActivityTaskManagerService"),int.class,IApplicationThread.class},atm,873501,thread);
        set(process,"uid",12002);set(process,"packageName",CLIENT);set(process,"processName",CLIENT);set(process,"ready",true);
        ((List<Object>)get(atm,"mProcesses")).add(process);
        IIntentReceiver r=new IIntentReceiver.Default(){final Binder b=new Binder();public IBinder asBinder(){return b;}};
        IntentFilter filter=new IntentFilter();filter.addAction(AccountManager.ACTION_ACCOUNT_REMOVED);filter.addAction(AccountManager.ACTION_VISIBLE_ACCOUNTS_CHANGED);filter.addAction(AccountManager.LOGIN_ACCOUNTS_CHANGED_ACTION);
        call(bc,"registerReceiver",new Class<?>[]{IApplicationThread.class,String.class,IIntentReceiver.class,IntentFilter.class,int.class,String.class,int.class},thread,CLIENT,r,filter,873501,null,Context.RECEIVER_EXPORTED);
      }
      void retireOwner()throws Exception{begin();call(pm,"remove",new Class<?>[]{String.class},OWNER);end();reconcile(false);}
      public void close()throws Exception{queue.shutdownNow();((ExecutorService)get(accounts,"mSessionTimeout")).shutdownNow();((ExecutorService)get(pm,"mPackageEventExecutor")).shutdownNow();((ExecutorService)get(bc,"mTimeouts")).shutdownNow();}
    }
    static void basic()throws Exception {
      try(H h=new H()) {
        Properties before=h.durable;h.fail=true;
        try{h.begin();throw new AssertionError("marker failure ignored");}catch(IllegalStateException expected){check(expected.getCause() instanceof IOException,"marker cause");}
        check(h.durable==before,"failed marker changed store");check(h.entries.size()==2,"failed marker removed accounts");h.fail=false;
        call(h.accounts,"packageRemoved",new Class<?>[]{String.class},OWNER);
        check(!h.access(REMOVED)&&h.access(RETAINED),"retirement fencing");h.end();h.reconcile(true);
        check(h.access(REMOVED)&&h.access(RETAINED),"rollback visibility");check("0".equals(h.durable.getProperty("pendingRemoval.count")),"rollback marker");
        h.begin();call(h.pm,"remove",new Class<?>[]{String.class},OWNER);h.end();
        call(h.accounts,"setSyncRetirement",new Class<?>[]{BiConsumer.class},(BiConsumer<Set<Account>,Set<Account>>)(removed,retained)->{check(removed.equals(Set.of(REMOVED)),"removed scope");check(retained.equals(Set.of(RETAINED)),"retained scope");throw new IllegalStateException("fixture sync failure");});
        try{h.reconcile(false);throw new AssertionError("sync failure ignored");}catch(IllegalStateException expected){check(expected.getMessage().equals("fixture sync failure"),"sync error");}
        check(h.entries.size()==2&&!h.access(REMOVED)&&h.access(RETAINED),"sync failure fence");
        call(h.accounts,"setSyncRetirement",new Class<?>[]{BiConsumer.class},(BiConsumer<Set<Account>,Set<Account>>)(removed,retained)->{});
        h.fail=true;before=h.durable;try{h.reconcile(false);throw new AssertionError("account failure ignored");}catch(IllegalStateException expected){check(expected.getCause() instanceof IOException,"account error");}
        check(h.durable==before&&h.entries.size()==2&&!h.access(REMOVED),"account failure fence/retention");h.fail=false;h.reconcile(false);
        check(!h.entries.containsKey(REMOVED)&&h.entries.containsKey(RETAINED),"successful cleanup isolation");check(!h.durable.containsValue("fixture-token")||h.durable.getProperty("count").equals("1"),"one retained credential");
        check("0".equals(h.durable.getProperty("pendingRemoval.count")),"successful marker cleanup");
        call(h.pm,"add",new Class<?>[]{t("ApkPackage")},pkg(OWNER,12001));check(!h.entries.containsKey(REMOVED),"identical reinstall revived account");
      }
      System.out.println("basic checks="+checks);
    }
    static void notification(boolean reinstall,boolean changedSigner)throws Exception {
      try(H h=new H()) {
        h.retireOwner();check(h.queue.tasks.size()==1,"expected one queued removal");
        if(reinstall){
          call(h.accounts,"packageRemoved",new Class<?>[]{String.class},CLIENT);call(h.pm,"remove",new Class<?>[]{String.class},CLIENT);
          Object replacement=pkg(CLIENT,12002);if(changedSigner)set(replacement,"mVerifiedSignatures",new Signature[]{new Signature(new byte[]{8})});
          call(h.pm,"add",new Class<?>[]{t("ApkPackage")},replacement);
        }
        h.connectReceiver();h.queue.drain();
        long removed=h.thread.events.stream().filter(i->AccountManager.ACTION_ACCOUNT_REMOVED.equals(i.getAction())).count();
        System.out.println("notification reinstall="+reinstall+" changedSigner="+changedSigner+" events="+h.thread.events.size()+" removed="+removed+" account="+(removed==0?"none":h.thread.events.get(h.thread.events.size()-1).getStringExtra(AccountManager.KEY_ACCOUNT_NAME)));
        check(removed==(reinstall?0:1),"notification reached a different installation lifetime");
      }
    }
    static void update() throws Exception {
      try (H h = new H()) {
        h.retireOwner();
        call(h.pm, "add", new Class<?>[]{t("ApkPackage")}, pkg(CLIENT, 12002));
        h.connectReceiver(); h.queue.drain();
        check(h.thread.events.stream().filter(i -> AccountManager.ACTION_ACCOUNT_REMOVED.equals(i.getAction())).count() == 1,
            "Safe same-signer update lost its removal event");
      }
    }
    static void handoff() throws Exception {
      try (H h = new H()) {
        h.retireOwner(); h.connectReceiver();
        set(h.bc, "mBeforeSenderAuthentication", (Runnable) () -> {
          try {
            set(h.bc, "mBeforeSenderAuthentication", null);
            call(h.accounts, "packageRemoved", new Class<?>[]{String.class}, CLIENT);
            call(h.pm, "remove", new Class<?>[]{String.class}, CLIENT);
            call(h.pm, "add", new Class<?>[]{t("ApkPackage")}, pkg(CLIENT, 12002));
            ((List<?>)get(h.atm, "mProcesses")).remove(h.process);
            h.connectReceiver();
          } catch (Exception failure) { throw new AssertionError(failure); }
        });
        h.queue.drain();
        check(h.thread.events.isEmpty(), "Removal authority survived reinstall during broadcast handoff");
      }
    }
    static void serialBatch() throws Exception {
      try (H h = new H()) {
        for (int i = 1; i < 16; i++) h.entry(new Account("retired-" + i, REMOVED.type), OWNER);
        h.save(); h.retireOwner();
        check(h.queue.tasks.size() == 1, "Retirement scheduled one worker per account");
        h.connectReceiver(); h.queue.drain();
        check(h.thread.events.stream().filter(i -> AccountManager.ACTION_ACCOUNT_REMOVED.equals(i.getAction())).count() == 16,
            "Serial delivery discarded account-removal events");
        check(h.queue.tasks.isEmpty(), "Completed notification work remained queued");
      }
    }
    static void concurrentSyncAdmission() throws Exception {
      try (H h = new H()) {
        Object identities = get(h.accounts, "mCodeIdentities");
        FutureTask<Boolean> access = new FutureTask<>(() -> h.access(RETAINED));
        Thread worker = new Thread(access, "sync-admission");
        worker.setDaemon(true);
        synchronized (identities) {
          worker.start();
          long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
          while (worker.getState() != Thread.State.BLOCKED && System.nanoTime() < deadline) {
            Thread.sleep(1);
          }
          check(worker.getState() == Thread.State.BLOCKED, "Sync access did not reach code identity lookup");
          call(h.pm, "beginPackageRemoval", new Class<?>[]{t("ApkPackage")}, h.client);
        }
        check(!access.get(5, TimeUnit.SECONDS), "Sync access survived concurrent client removal");
      }
    }
    public static void main(String[] args)throws Exception {
      System.load(args[0]); PropertyInvalidatedCache.setTestMode(true);
      basic(); notification(false, false); notification(true, true); notification(true, false);
      update(); handoff(); serialBatch(); concurrentSyncAdmission();
      System.out.println("Account retirement contracts passed: " + checks + " checks; actual production services, declared host boundaries");
    }
}
