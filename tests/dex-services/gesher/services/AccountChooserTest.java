/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.accounts.Account;
import android.accounts.AccountManager;
import android.app.IApplicationThread;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.res.AssetManager;
import android.os.Binder;
import android.os.IBinder;
import java.lang.reflect.*;
import java.nio.file.Files;
import java.util.*;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.*;
import java.util.function.BooleanSupplier;

/** Synthetic accounts only: trusted-dialog choices, code policy and authenticated ATM route. */
public final class AccountChooserTest {
    private static int checks;
    private static void require(boolean value, String reason) {
        if (!value) throw new AssertionError(reason); checks++;
    }
    private static Field field(Class<?> type, String name) throws Exception {
        Field f = type.getDeclaredField(name); f.setAccessible(true); return f;
    }
    private static Object get(Object o, String name) throws Exception { return field(o.getClass(), name).get(o); }
    private static void set(Object o, String name, Object value) throws Exception { field(o.getClass(), name).set(o, value); }
    private static int pending(Object tasks) throws Exception {
        synchronized(tasks) { return ((Map<?,?>)get(tasks,"mAccountChoosers")).size(); }
    }
    private static int results(Object tasks, Object source) throws Exception {
        synchronized(tasks) { return ((List<?>)get(source,"pendingResults")).size(); }
    }
    private static Method method(Class<?> type, String name, Class<?>... args) throws Exception {
        Method m = type.getDeclaredMethod(name, args); m.setAccessible(true); return m;
    }
    private static Object make(Class<?> type, Class<?>[] args, Object... values) throws Exception {
        Constructor<?> c = type.getDeclaredConstructor(args); c.setAccessible(true); return c.newInstance(values);
    }
    private static void denied(Method method, Object receiver, Object... args) throws Exception {
        try { method.invoke(receiver, args); }
        catch (InvocationTargetException e) { if (e.getCause() instanceof SecurityException) { checks++; return; } throw e; }
        throw new AssertionError("Forged chooser source accepted");
    }
    private static final class AppThread extends IApplicationThread.Default {
        final Binder binder = new Binder();
        @Override public IBinder asBinder() { return binder; }
    }
    private static final String TYPE = "com.gesher.devsvc.account";
    private static final Class<?>[] NO_ARGS = {};
    private static Object manager, packages, caller;
    private static Class<?> service, sessionType, entryType, chooserType, lifetimeType, launcherType, resultType, dialogType;
    private static String clientName, ownerName, clientKey, ownerCode;
    private static final AtomicBoolean alive = new AtomicBoolean(true);
    private static final AtomicInteger writes = new AtomicInteger();
    private static Object lifetime;
    @SuppressWarnings("unchecked") private static Map<Account,Object> records() throws Exception {
        return (Map<Account,Object>) get(manager, "mAccounts");
    }
    @SuppressWarnings("unchecked") private static Map<String,Integer> visibility(Object entry) throws Exception {
        return (Map<String,Integer>) get(entry, "visibility");
    }
    private static Object account(Account account, Integer visibility) throws Exception {
        Object entry = make(entryType, NO_ARGS);
        set(entry,"owner",ownerName); set(entry,"ownerIdentity",ownerCode);
        if (visibility != null) visibility(entry).put(clientKey, visibility);
        records().put(account, entry); return entry;
    }
    private static Object session(Intent request) throws Exception {
        return make(sessionType, new Class<?>[]{service, caller.getClass(), Intent.class, Runnable.class},
                manager, caller, request, (Runnable) writes::incrementAndGet);
    }
    @SuppressWarnings("unchecked") private static List<Account> candidates(Object session) throws Exception {
        return (List<Account>) method(sessionType,"accounts").invoke(session);
    }
    private static Intent select(Object session, Account account) throws Exception {
        return (Intent) method(sessionType,"select",Account.class,lifetimeType).invoke(session,account,lifetime);
    }
    private static Intent run(Object session, java.util.function.ToIntFunction<Object[]> choice,
            AtomicInteger dialogs, AtomicInteger launches) throws Exception {
        AtomicReference<Intent> result = new AtomicReference<>();
        AtomicInteger finished = new AtomicInteger();
        Object dialog = Proxy.newProxyInstance(dialogType.getClassLoader(),new Class<?>[]{dialogType},
                (p,m,a)-> { dialogs.incrementAndGet(); return choice.applyAsInt(a); });
        Object launcher = Proxy.newProxyInstance(launcherType.getClassLoader(),new Class<?>[]{launcherType},
                (p,m,a)-> { launches.incrementAndGet(); return false; });
        Object receiver = Proxy.newProxyInstance(resultType.getClassLoader(),new Class<?>[]{resultType},
                (p,m,a)-> { finished.incrementAndGet(); result.set((Intent)a[0]); return null; });
        Object chooser = make(chooserType,new Class<?>[]{sessionType,lifetimeType,launcherType,resultType,dialogType},
                session,lifetime,launcher,receiver,dialog);
        method(chooserType,"run").invoke(chooser);
        require(finished.get()==1,"Chooser result was not delivered exactly once");
        return result.get();
    }

    public static void main(String[] args) throws Exception {
        require(args.length==2,"Pass metadata-only authenticator and client fixture APKs");
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false,System.getenv("ANDROID_ROOT")+"/framework/framework-res.apk");
        android.system.Os.setenv("ANDROID_DATA",Files.createTempDirectory("chooser-fixture-").toString(),true);
        android.system.Os.setenv("GESHER_MICROG_SPOOF","1",true);
        Class<?> apk=Class.forName("com.gesher.server.ApkPackage");
        Object owner=apk.getMethod("parse",String.class).invoke(null,args[0]);
        Object client=apk.getMethod("parse",String.class).invoke(null,args[1]);
        ownerName=(String)apk.getField("packageName").get(owner); clientName=(String)apk.getField("packageName").get(client);
        Class<?> pm=Class.forName("com.gesher.server.PackageManagerService");
        Class<?> atm=Class.forName("com.gesher.server.ActivityTaskManagerService");
        Class<?> am=Class.forName("com.gesher.server.ActivityManagerService");
        packages=pm.getConstructor(Collection.class).newInstance(Arrays.asList(owner,client));
        service=Class.forName("com.gesher.server.AccountManagerService");
        manager=service.getConstructor(am,pm).newInstance(null,packages);
        Class<?> callerType=Class.forName(service.getName()+"$Caller");
        ownerCode=(String)method(service,"packageIdentity",String.class).invoke(manager,ownerName);
        String clientCode=(String)method(service,"packageIdentity",String.class).invoke(manager,clientName);
        int clientUid=(Integer)method(apk,"uid").invoke(client);
        caller=make(callerType,new Class<?>[]{String.class,String.class,boolean.class,int.class,int.class},clientName,clientCode,false,clientUid,123);
        clientKey=(String)method(callerType,"key").invoke(caller);
        sessionType=Class.forName(service.getName()+"$ChooserSession"); entryType=Class.forName(service.getName()+"$Entry");
        chooserType=Class.forName("com.gesher.server.AccountChooser");
        lifetimeType=Class.forName(chooserType.getName()+"$Lifetime"); launcherType=Class.forName(chooserType.getName()+"$Launcher");
        resultType=Class.forName(chooserType.getName()+"$Result"); dialogType=Class.forName(chooserType.getName()+"$Dialog");
        lifetime=Proxy.newProxyInstance(lifetimeType.getClassLoader(),new Class<?>[]{lifetimeType},(p,m,a)->{
            if (!alive.get()) return false;
            if (m.getName().equals("whileActive")) ((Runnable)a[0]).run();
            return true;
        });
        records().clear();
        Account unset=new Account("unset@example.test",TYPE), deny=new Account("deny@example.test",TYPE);
        Account hidden=new Account("manual@example.test",TYPE), visible=new Account("visible@example.test",TYPE);
        Object unsetEntry=account(unset,null); account(deny,3); account(hidden,4); account(visible,1);
        Account stale=new Account("stale@example.test",TYPE), foreign=new Account("foreign@example.test",TYPE);
        set(account(stale,null),"ownerIdentity","old-code"); set(account(foreign,null),"owner",clientName);
        Object unrestricted=session(new Intent());
        require(new HashSet<>(candidates(unrestricted)).equals(new HashSet<>(Arrays.asList(unset,hidden,visible))),
                "Chooser leaked explicit-denied, stale or foreign-owned accounts, or hid unset policy");
        require(!(Boolean)method(service,"visible",callerType,Account.class,entryType).invoke(manager,caller,unset,unsetEntry),
                "Offering an unset account changed ordinary visibility");
        require(candidates(session(new Intent().putExtra("allowableAccountTypes",new String[0]))).isEmpty(),"Empty type filter ignored");
        Intent filter=new Intent().putParcelableArrayListExtra("allowableAccounts",new ArrayList<>(Arrays.asList(hidden)));
        require(candidates(session(filter)).equals(Arrays.asList(hidden)),"Allowed account filter ignored");
        require(candidates(session(new Intent().putParcelableArrayListExtra("allowableAccounts",new ArrayList<Account>()))).isEmpty(),
                "Empty allowed account list ignored");
        ArrayList<Account> mutableFilter=new ArrayList<>(Arrays.asList(unset));
        Object capturedFilter=session(new Intent().putParcelableArrayListExtra("allowableAccounts",mutableFilter));
        mutableFilter.clear(); mutableFilter.add(deny);
        require(candidates(capturedFilter).equals(Arrays.asList(unset)),"Caller changed an asynchronous filter snapshot");
        AtomicInteger dialogs=new AtomicInteger(),launches=new AtomicInteger();
        Intent one=new Intent().putParcelableArrayListExtra("allowableAccounts",new ArrayList<>(Arrays.asList(unset)))
                .putExtra("selectedAccount",unset);
        require(run(session(one),a->{require(((String[])a[1]).length==1,"Single candidate lost");return -1;},dialogs,launches)==null,
                "Cancel returned an account");
        require(dialogs.get()==1 && writes.get()==0 && visibility(unsetEntry).isEmpty(),"Single candidate was selected or granted automatically");
        require(run(session(one),a->99,new AtomicInteger(),launches)==null && writes.get()==0,"Out-of-range result selected an account");
        require(run(session(one),a->{alive.set(false);return 0;},new AtomicInteger(),launches)==null && writes.get()==0,
                "Dead source received visibility"); alive.set(true);
        Object guarded=session(one); candidates(guarded);
        Object lostLifetime=Proxy.newProxyInstance(lifetimeType.getClassLoader(),new Class<?>[]{lifetimeType},
                (p,m,a)->m.getName().equals("active"));
        require(method(sessionType,"select",Account.class,lifetimeType).invoke(guarded,unset,lostLifetime)==null
                && writes.get()==0 && visibility(unsetEntry).isEmpty(),"Source death at final commit check granted visibility");
        require(run(session(one),a->{
            try { visibility(unsetEntry).put(clientKey,3); } catch(Exception e){ throw new AssertionError(e); }
            return 0;
        },new AtomicInteger(),launches)==null && writes.get()==0,"Owner denial during dialog was overridden");
        visibility(unsetEntry).clear();
        Object s=session(one); candidates(s);
        Object replacement=account(unset,null);
        require(select(s,unset)==null,"Removed/recreated account inherited an in-flight choice");
        records().put(unset,unsetEntry);
        Intent selected=run(session(one),a->0,new AtomicInteger(),launches);
        require(selected!=null && unset.name.equals(selected.getStringExtra(AccountManager.KEY_ACCOUNT_NAME))
                && TYPE.equals(selected.getStringExtra(AccountManager.KEY_ACCOUNT_TYPE)),"Public chooser result contract changed");
        require(writes.get()==1 && visibility(unsetEntry).get(clientKey)==2,"Explicit selection did not persist per-code user visibility");
        require(visibility(records().get(deny)).get(clientKey)==3 && visibility(records().get(hidden)).get(clientKey)==4
                && visibility(records().get(visible)).get(clientKey)==1,"Selection changed another account's policy");
        require(!(Boolean)method(service,"tokenAuthorized",callerType,entryType,String.class).invoke(manager,caller,unsetEntry,"scope"),
                "Account selection authorized tokens");
        require(((Map<?,?>)get(unsetEntry,"tokens")).isEmpty() && ((Map<?,?>)get(unsetEntry,"tokenGrants")).isEmpty()
                && (Long)get(unsetEntry,"authenticatedAt")==0L,"Chooser changed credentials/authentication state");
        require(run(session(filter),a->0,new AtomicInteger(),launches)!=null
                && visibility(records().get(hidden)).get(clientKey)==2 && writes.get()==2,
                "Explicit user choice did not reveal a user-managed hidden account");
        set(caller,"code","replacement-code");
        require(candidates(session(one)).isEmpty(),"Replacement client inherited chooser access"); set(caller,"code",clientCode);
        require(launches.get()==0,"A selection or cancellation launched Add");
        AtomicInteger addDialogs=new AtomicInteger();
        require(run(session(new Intent()),a->{
            int n=addDialogs.incrementAndGet();
            if(n==1){require((Boolean)a[2],"Registered type cannot be added");return -2;}
            require(!(Boolean)a[2],"Type picker offered recursive Add");return -1;
        },new AtomicInteger(),launches)==null && addDialogs.get()==2 && launches.get()==0,"Add did not require a separate explicit type choice");

        // Exercise the actual platform-component route with an attached synthetic activity.
        Object tasks=make(atm,new Class<?>[]{pm},packages);
        Class<?> notification=Class.forName("com.gesher.server.NotificationManagerService");
        Object activityManager=make(am,new Class<?>[]{atm,pm,notification},tasks,packages,make(notification,NO_ARGS));
        Object routedManager=service.getConstructor(am,pm).newInstance(activityManager,packages);
        require(get(tasks,"mAccounts")==routedManager,"Account service was not attached to ATM");
        Class<?> processType=Class.forName(atm.getName()+"$ProcessRecord");
        AppThread thread=new AppThread();
        Object process=make(processType,new Class<?>[]{atm,int.class,IApplicationThread.class},tasks,Binder.getCallingPid(),thread);
        set(process,"uid",clientUid);
        set(process,"packageName",clientName); set(process,"processName",clientName);
        ((List<Object>)get(tasks,"mProcesses")).add(process);
        ActivityInfo info=new ActivityInfo(); info.name=clientName+".ChooserSource"; info.packageName=clientName;
        Class<?> recordType=Class.forName(atm.getName()+"$ActivityRecord");
        Object source=make(recordType,new Class<?>[]{atm,ActivityInfo.class,Intent.class,int.class,int.class,String.class,recordType,String.class,int.class},
                tasks,info,new Intent(),1,Binder.getCallingPid(),clientName,null,null,-1);
        set(source,"app",process); Class<?> state=Class.forName(atm.getName()+"$State");
        set(source,"state",Enum.valueOf((Class)state,"PAUSED"));
        ((List<Object>)get(tasks,"mActivities")).add(source);
        IBinder token=(IBinder)get(source,"token");
        Method route=method(atm,"chooseAccount",IApplicationThread.class,String.class,Intent.class,IBinder.class,String.class,int.class);
        Intent request=new Intent().setComponent(new ComponentName("android","android.accounts.ChooseTypeAndAccountActivity"));
        require((Boolean)method(chooserType,"handles",Intent.class).invoke(null,request),"Public AccountManager component not intercepted");
        require(!(Boolean)method(chooserType,"handles",Intent.class).invoke(null,new Intent().setComponent(new ComponentName(clientName,"android.accounts.ChooseTypeAndAccountActivity"))),
                "Foreign lookalike component intercepted");
        denied(route,tasks,thread,clientName,request,new Binder(),null,4);
        denied(route,tasks,new AppThread(),clientName,request,token,null,4);
        denied(route,tasks,thread,ownerName,request,token,null,4);
        denied(route,tasks,thread,clientName,request,token,null,-1);
        int originalPid=(Integer)get(process,"pid"); set(process,"pid",originalPid+100000);
        denied(route,tasks,thread,clientName,request,token,null,4); set(process,"pid",originalPid);
        set(process,"exiting",true); denied(route,tasks,thread,clientName,request,token,null,4); set(process,"exiting",false);
        set(source,"finishing",true); denied(route,tasks,thread,clientName,request,token,null,4); set(source,"finishing",false);
        CountDownLatch shown=new CountDownLatch(1),release=new CountDownLatch(1);
        Object dialog=Proxy.newProxyInstance(dialogType.getClassLoader(),new Class<?>[]{dialogType},(p,m,a)->{
            shown.countDown(); release.await(5,TimeUnit.SECONDS); return -1;
        });
        set(tasks,"mAccountDialog",dialog);
        int started=((android.app.IActivityTaskManager)tasks).startActivity(thread,clientName,null,
                request,null,token,null,42,0,null,null);
        require(started==0,"Public startActivity route did not reach platform chooser");
        require(shown.await(5,TimeUnit.SECONDS),"Chooser blocked before trusted dialog");
        require(pending(tasks)==1,"Pending chooser not retained");
        release.countDown();
        long until=System.currentTimeMillis()+5000;
        while(results(tasks,source)==0 && System.currentTimeMillis()<until) Thread.sleep(10);
        List<?> results=(List<?>)get(source,"pendingResults");
        require(results.size()==1,"Cancel did not return exactly one activity result");
        Object response=results.get(0);
        require((Integer)response.getClass().getField("mRequestCode").get(response)==42
                && (Integer)response.getClass().getField("mResultCode").get(response)==0,"Cancellation result contract changed");
        require(pending(tasks)==0,"Completed chooser leaked a reservation");
        // A source disappearing after its dialog opened neither receives a result nor
        // keeps a pending reservation. Test the real ATM lifetime predicate, not extras.
        AtomicInteger lifetimeChecks=new AtomicInteger();
        Object deadDialog=Proxy.newProxyInstance(dialogType.getClassLoader(),new Class<?>[]{dialogType},(p,m,a)->{
            set(source,"finishing",true);
            require(!((BooleanSupplier)a[3]).getAsBoolean(),"Finishing source remained active in trusted dialog");
            lifetimeChecks.incrementAndGet(); return 0;
        });
        set(tasks,"mAccountDialog",deadDialog);
        ((android.app.IActivityTaskManager)tasks).startActivity(thread,clientName,null,request,null,token,null,43,0,null,null);
        until=System.currentTimeMillis()+5000;
        while(pending(tasks)!=0 && System.currentTimeMillis()<until) Thread.sleep(10);
        require(lifetimeChecks.get()==1 && results.size()==1,"Finishing source received a chooser result");
        require(pending(tasks)==0,"Dead source leaked its chooser reservation");
        set(source,"finishing",false);
        Object codeDialog=Proxy.newProxyInstance(dialogType.getClassLoader(),new Class<?>[]{dialogType},(p,m,a)->{
            Object newCode=apk.getMethod("parse",String.class).invoke(null,args[1]);
            method(pm,"add",apk).invoke(packages,newCode);
            require(!((BooleanSupplier)a[3]).getAsBoolean(),"Replaced package retained an in-flight chooser");
            lifetimeChecks.incrementAndGet(); return 0;
        });
        set(tasks,"mAccountDialog",codeDialog);
        ((android.app.IActivityTaskManager)tasks).startActivity(thread,clientName,null,request,null,token,null,44,0,null,null);
        until=System.currentTimeMillis()+5000;
        while(pending(tasks)!=0 && System.currentTimeMillis()<until) Thread.sleep(10);
        require(lifetimeChecks.get()==2 && results.size()==1,"Replaced client received a chooser result");
        require(pending(tasks)==0,"Replaced source leaked its chooser reservation");
        AtomicInteger continuation=new AtomicInteger(); set(source,"accountCompletion",(Runnable)continuation::incrementAndGet);
        method(atm,"accountActivityFinished",recordType).invoke(null,source);
        method(atm,"accountActivityFinished",recordType).invoke(null,source);
        require(continuation.get()==1 && results.size()==1,"Authenticator continuation replayed or leaked activity result data");
        System.out.println("AccountChooserTest all passed: "+checks+" synthetic policy, explicit-choice and authenticated-route checks");
    }
}
