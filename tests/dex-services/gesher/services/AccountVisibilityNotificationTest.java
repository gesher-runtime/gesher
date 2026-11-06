/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.accounts.Account;
import android.accounts.AccountManager;
import android.app.PropertyInvalidatedCache;
import android.content.Intent;
import android.content.res.AssetManager;
import java.lang.reflect.*;
import java.nio.file.Files;
import java.util.*;
import java.util.concurrent.atomic.AtomicInteger;

/** Synthetic visibility commits: cache nonce, targeted events, no-op and rollback. */
public final class AccountVisibilityNotificationTest {
    private static int checks;
    private static void require(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
        ++checks;
    }
    private static Field field(Class<?> type, String name) throws Exception {
        Field f=type.getDeclaredField(name); f.setAccessible(true); return f;
    }
    private static Method method(Class<?> type, String name, Class<?>... args) throws Exception {
        Method m=type.getDeclaredMethod(name,args); m.setAccessible(true); return m;
    }
    private static Object make(Class<?> type, Class<?>[] types, Object... args) throws Exception {
        Constructor<?> c=type.getDeclaredConstructor(types); c.setAccessible(true); return c.newInstance(args);
    }
    private static Object get(Object target,String name) throws Exception {
        return field(target.getClass(),name).get(target);
    }
    private static void set(Object target,String name,Object value) throws Exception {
        field(target.getClass(),name).set(target,value);
    }
    private static final String TYPE="com.gesher.devsvc.account";
    private static final String CACHE="cache_key.system_server.accounts_data";
    private static Object manager,packages,caller;
    private static Class<?> service,entryType,sessionType,lifetimeType;
    private static String ownerName,ownerCode,clientName,clientCode,clientKey;
    private static List<Boolean> transitions=new ArrayList<>();
    private static AtomicInteger commits=new AtomicInteger();
    private static PropertyInvalidatedCache<Integer,Integer> cache;
    private static PropertyInvalidatedCache<Integer,Integer> userDataCache;
    private static AtomicInteger recomputes=new AtomicInteger();

    @SuppressWarnings("unchecked") private static Map<Account,Object> accounts() throws Exception {
        return (Map<Account,Object>)get(manager,"mAccounts");
    }
    @SuppressWarnings("unchecked") private static Map<String,Integer> grants(Object entry) throws Exception {
        return (Map<String,Integer>)get(entry,"visibility");
    }
    private static Object account(Account account,Integer value) throws Exception {
        Object entry=make(entryType,new Class<?>[0]);
        set(entry,"owner",ownerName); set(entry,"ownerIdentity",ownerCode);
        if(value!=null) grants(entry).put(clientKey,value);
        accounts().put(account,entry); return entry;
    }
    private static Object session(Account account) throws Exception {
        Intent request=new Intent().putParcelableArrayListExtra("allowableAccounts",
                new ArrayList<>(Arrays.asList(account)));
        Runnable persisted=()-> {
            commits.incrementAndGet();
            try { set(manager,"mBeforeMutation",null); }
            catch(Exception e) { throw new AssertionError(e); }
        };
        Object s=make(sessionType,new Class<?>[]{service,caller.getClass(),Intent.class,Runnable.class},
                manager,caller,request,persisted);
        method(sessionType,"accounts").invoke(s);
        return s;
    }
    private static Intent select(Object s,Account account,boolean active) throws Exception {
        Object lifetime=Proxy.newProxyInstance(lifetimeType.getClassLoader(),new Class<?>[]{lifetimeType},
                (p,m,a)-> { if(!active)return false; if(m.getName().equals("whileActive"))((Runnable)a[0]).run(); return true; });
        return (Intent)method(sessionType,"select",Account.class,lifetimeType).invoke(s,account,lifetime);
    }
    private static void capture() throws Exception {
        Class<?> notifier=Class.forName(service.getName()+"$VisibilityNotifier");
        set(manager,"mVisibilityNotifier",Proxy.newProxyInstance(notifier.getClassLoader(),new Class<?>[]{notifier},
                (p,m,a)-> {
                    require(clientName.equals(a[0]) && clientCode.equals(a[1]),"Notification escaped original client code");
                    transitions.add((Boolean)a[2]); return null;
                }));
    }
    public static void main(String[] args) throws Exception {
        require(args.length==2,"Pass synthetic authenticator/client APKs");
        System.loadLibrary("android_runtime");
        // Android16 account nonces use ApplicationSharedMemory. This isolated
        // in-process service fixture owns a writable server region for its VM.
        Class<?> shared=Class.forName("com.android.internal.os.ApplicationSharedMemory");
        Object region=shared.getMethod("create").invoke(null);
        shared.getMethod("setInstance",shared).invoke(null,region);
        cache=new PropertyInvalidatedCache<Integer,Integer>(4,CACHE) {
            @Override public Integer recompute(Integer query) { return recomputes.incrementAndGet(); }
        };
        userDataCache=new PropertyInvalidatedCache<Integer,Integer>(4,"cache_key.system_server.account_user_data") {
            @Override public Integer recompute(Integer query) { return recomputes.incrementAndGet(); }
        };
        AccountManager.invalidateLocalAccountsDataCaches();
        AccountManager.invalidateLocalAccountUserDataCaches();
        int warm=cache.query(0);
        require(cache.query(0)==warm,"Framework nonce cache did not retain its warm query");
        AssetManager.createSystemAssetsInZygoteLocked(false,System.getenv("ANDROID_ROOT")+"/framework/framework-res.apk");
        android.system.Os.setenv("ANDROID_DATA",Files.createTempDirectory("account-notify-").toString(),true);
        android.system.Os.setenv("GESHER_MICROG_SPOOF","1",true);
        Class<?> apk=Class.forName("com.gesher.server.ApkPackage");
        Object owner=apk.getMethod("parse",String.class).invoke(null,args[0]);
        Object client=apk.getMethod("parse",String.class).invoke(null,args[1]);
        ownerName=(String)apk.getField("packageName").get(owner); clientName=(String)apk.getField("packageName").get(client);
        Class<?> pm=Class.forName("com.gesher.server.PackageManagerService");
        Class<?> am=Class.forName("com.gesher.server.ActivityManagerService");
        packages=pm.getConstructor(Collection.class).newInstance(Arrays.asList(owner,client));
        service=Class.forName("com.gesher.server.AccountManagerService");
        manager=service.getConstructor(am,pm).newInstance(null,packages);
        ownerCode=(String)method(service,"packageIdentity",String.class).invoke(manager,ownerName);
        clientCode=(String)method(service,"packageIdentity",String.class).invoke(manager,clientName);
        Class<?> callerType=Class.forName(service.getName()+"$Caller");
        caller=make(callerType,new Class<?>[]{String.class,String.class,boolean.class,int.class,int.class},
                clientName,clientCode,false,(Integer)method(apk,"uid").invoke(client),123);
        clientKey=(String)method(callerType,"key").invoke(caller);
        entryType=Class.forName(service.getName()+"$Entry");
        sessionType=Class.forName(service.getName()+"$ChooserSession");
        lifetimeType=Class.forName("com.gesher.server.AccountChooser$Lifetime");
        accounts().clear(); capture();
        Account first=new Account("first@example.test",TYPE); Object entry=account(first,null);
        long nonce=cache.getNonce();
        require(select(session(first),first,true)!=null,"Committed selection did not return account");
        require(grants(entry).get(clientKey)==2 && commits.get()==1,"Committed selection did not persist");
        require(transitions.equals(Arrays.asList(true)),"Hidden-to-visible selection was not announced");
        require(cache.getNonce()!=nonce && cache.query(0)>warm,"Account list cache was not invalidated");
        nonce=cache.getNonce();
        require(select(session(first),first,true)!=null,"Repeated selection failed");
        require(transitions.size()==1,"Unchanged stored visibility repeated notifications");
        require(cache.getNonce()!=nonce,"No-op visibility update retained account cache");
        Account visible=new Account("visible@example.test",TYPE); account(visible,1);
        require(select(session(visible),visible,true)!=null,"Visible account could not be selected");
        require(transitions.equals(Arrays.asList(true,false)),"Visible-to-visible value change announced a visibility transition");
        Account hidden=new Account("hidden@example.test",TYPE); account(hidden,4);
        require(select(session(hidden),hidden,true)!=null && transitions.equals(Arrays.asList(true,false,true)),
                "User-managed hidden account did not announce transition");
        int events=transitions.size(),writes=commits.get(); nonce=cache.getNonce();
        Account denied=new Account("denied@example.test",TYPE); account(denied,3);
        require(select(session(denied),denied,true)==null,"Explicit deny was bypassed");
        Account dead=new Account("dead@example.test",TYPE); account(dead,null);
        require(select(session(dead),dead,false)==null,"Dead source selected an account");
        require(transitions.size()==events && commits.get()==writes && cache.getNonce()==nonce,
                "Denied or canceled choice notified/invalidate/persisted");

        // Exercise the public setter's real persistence rollback: a normal test
        // main is intentionally denied by the actual server-only DPAPI bridge.
        manager=service.getConstructor(am,pm).newInstance(null,packages);
        accounts().clear(); capture(); account(first,3);
        nonce=cache.getNonce(); events=transitions.size();
        boolean failed=false;
        try { service.getMethod("setAccountVisibility",Account.class,String.class,int.class).invoke(manager,first,clientName,2); }
        catch(InvocationTargetException e) { failed=e.getCause() instanceof IllegalStateException; }
        require(failed,"Non-server fixture unexpectedly persisted protected account state");
        require(grants(accounts().get(first)).get(clientKey)==3,"Persistence failure did not roll back visibility");
        require(transitions.size()==events && cache.getNonce()==nonce,
                "Failed persistence announced or invalidated an uncommitted grant");
        // The existing add/remove/rename notification path must refresh both
        // enabled framework caches, even without a transport in this local VM.
        long dataNonce=userDataCache.getNonce(); nonce=cache.getNonce();
        method(service,"changed",Account.class,boolean.class,entryType).invoke(manager,first,false,accounts().get(first));
        require(cache.getNonce()!=nonce && userDataCache.getNonce()!=dataNonce,
                "Account lifecycle notification retained an enabled framework cache");
        dataNonce=userDataCache.getNonce();
        failed=false;
        try { service.getMethod("setUserData",Account.class,String.class,String.class).invoke(manager,first,"fixture-key","fixture-value"); }
        catch(InvocationTargetException e) { failed=e.getCause() instanceof IllegalStateException; }
        require(failed && userDataCache.getNonce()==dataNonce,
                "Failed user-data persistence invalidated an uncommitted change");
        System.out.println("AccountVisibilityNotificationTest all passed: "+checks+" synthetic checks");
    }
}
