/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.accounts.Account;
import android.accounts.AccountManager;
import android.content.ComponentName;
import android.content.res.AssetManager;
import android.os.Bundle;
import java.io.File;
import java.lang.reflect.*;
import java.nio.file.Files;
import java.util.*;
import java.util.function.Predicate;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.atomic.AtomicBoolean;

/** Synthetic uninstall capabilities and transactional serialization. Real DPAPI needs SystemServer. */
public final class AccountClientRemovalTest {
    private static int checks;
    private static void require(boolean okay, String why) { if (!okay) throw new AssertionError(why); checks++; }
    private static Field field(Class<?> c,String n)throws Exception { Field f=c.getDeclaredField(n);f.setAccessible(true);return f; }
    private static Object get(Object o,String n)throws Exception{return field(o.getClass(),n).get(o);}
    private static void set(Object o,String n,Object v)throws Exception{field(o.getClass(),n).set(o,v);}
    private static Method method(Class<?> c,String n,Class<?>...p)throws Exception{Method m=c.getDeclaredMethod(n,p);m.setAccessible(true);return m;}
    private static Object make(Class<?> c,Class<?>[] p,Object...v)throws Exception{Constructor<?> k=c.getDeclaredConstructor(p);k.setAccessible(true);return k.newInstance(v);}
    @SuppressWarnings("unchecked") private static Map<String,Integer> visibility(Object e)throws Exception{return(Map<String,Integer>)get(e,"visibility");}
    @SuppressWarnings("unchecked") private static Map<String,Set<String>> grants(Object e)throws Exception{return(Map<String,Set<String>>)get(e,"tokenGrants");}
    @SuppressWarnings("unchecked") private static Map<String,Object> custom(Object e)throws Exception{return(Map<String,Object>)get(e,"customTokens");}
    @SuppressWarnings("unchecked") private static Map<Account,Object> entries(Object m)throws Exception{return(Map<Account,Object>)get(m,"mAccounts");}
    private static Object entry(Class<?> type,String owner,String code)throws Exception{
        Object e=make(type,new Class<?>[0]);set(e,"owner",owner);set(e,"ownerIdentity",code);
        set(e,"password","synthetic-provider-secret");
        @SuppressWarnings("unchecked") Map<String,String> tokens=(Map<String,String>)get(e,"tokens");
        tokens.put("scope","synthetic-provider-token");return e;
    }
    private static Bundle issue(Object sessions,String client,int uid,String owner,String code)throws Exception{
        Bundle internal=new Bundle();internal.putString("synthetic","opaque");
        return(Bundle)method(sessions.getClass(),"issue",String.class,int.class,String.class,String.class,String.class,Account.class,Bundle.class)
                .invoke(sessions,client,uid,owner,code,"fixture.type",null,internal);
    }
    private static void deniedSession(Object sessions,Bundle handle,String key,int uid)throws Exception{
        try{method(sessions.getClass(),"consume",Bundle.class,String.class,int.class,Predicate.class)
                .invoke(sessions,handle,key,uid,(Predicate<Object>)p->true);throw new AssertionError("Removed session survived");}
        catch(InvocationTargetException e){require(e.getCause() instanceof SecurityException,"Wrong retired-session failure");}
    }
    public static void main(String[] args)throws Exception{
        require(args.length==2,"Pass synthetic authenticator and client APKs");
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false,System.getenv("ANDROID_ROOT")+"/framework/framework-res.apk");
        File data=Files.createTempDirectory("account-client-removal-").toFile();
        android.system.Os.setenv("ANDROID_DATA",data.getPath(),true);
        android.system.Os.setenv("GESHER_MICROG_SPOOF","1",true);
        android.system.Os.setenv("GESHER_BINDER_UID","1000",true);
        android.system.Os.setenv("GESHER_UID","1000",true);
        Class<?> apk=Class.forName("com.gesher.server.ApkPackage"),pm=Class.forName("com.gesher.server.PackageManagerService");
        Class<?> am=Class.forName("com.gesher.server.ActivityManagerService"),svc=Class.forName("com.gesher.server.AccountManagerService");
        Object owner=apk.getMethod("parse",String.class).invoke(null,args[0]);
        Object client=apk.getMethod("parse",String.class).invoke(null,args[1]);
        String ownerName=(String)apk.getField("packageName").get(owner),clientName=(String)apk.getField("packageName").get(client);
        int uid=(Integer)method(apk,"uid").invoke(client);
        int ownerUid=(Integer)method(apk,"uid").invoke(owner);
        Object packages=pm.getConstructor(Collection.class).newInstance(Arrays.asList(owner,client));
        Object manager=svc.getConstructor(am,pm).newInstance(null,packages);
        Class<?> writer=Class.forName(svc.getName()+"$StoreWriter");
        AtomicReference<Properties> persisted=new AtomicReference<>();AtomicBoolean failWrite=new AtomicBoolean();
        set(manager,"mStoreWriter",Proxy.newProxyInstance(writer.getClassLoader(),new Class<?>[]{writer},(p,m,a)->{
            if(failWrite.get())throw new java.io.IOException("Synthetic account-store write failure");
            Properties copy=new Properties();copy.putAll((Properties)a[0]);persisted.set(copy);return null;
        }));
        String ownerCode=(String)method(svc,"packageIdentity",String.class).invoke(manager,ownerName);
        String clientCode=(String)method(svc,"packageIdentity",String.class).invoke(manager,clientName);
        String clientKey=clientName+"\u0000"+clientCode,oldKey=clientName+"\u0000obsolete-signer",otherKey=ownerName+"\u0000"+ownerCode;
        Class<?> caller=Class.forName(svc.getName()+"$Caller"),entry=Class.forName(svc.getName()+"$Entry");
        Object oldCaller=method(svc,"captureCaller",String.class,String.class,boolean.class,int.class,int.class)
                .invoke(manager,clientName,clientCode,false,uid,123);
        entries(manager).clear();
        Class<?> auth=Class.forName(svc.getName()+"$Authenticator"),answer=Class.forName(svc.getName()+"$Answer");
        Object realOwner=method(svc,"authenticator",String.class).invoke(manager,"com.gesher.devsvc.account");
        require(realOwner!=null,"Synthetic authenticator unavailable for capture race");
        Object systemCaller=method(svc,"captureCaller",String.class,String.class,boolean.class,int.class,int.class)
                .invoke(manager,"android",null,true,1000,1);
        Method commit=method(svc,"commitOwnedAccount",caller,auth,Account.class,String.class,Bundle.class,Map.class);
        CountDownLatch entered=new CountDownLatch(1);AtomicReference<Throwable> outcome=new AtomicReference<>();
        Thread insertion=new Thread(()->{
            entered.countDown();
            try{commit.invoke(manager,systemCaller,realOwner,new Account("late@example.test","com.gesher.devsvc.account"),
                    "synthetic",null,null);outcome.set(new AssertionError("Retiring provider inserted an account"));}
            catch(InvocationTargetException failure){outcome.set(failure.getCause());}
            catch(Throwable failure){outcome.set(failure);}
        });
        synchronized(manager){
            insertion.start();require(entered.await(3,TimeUnit.SECONDS),"Commit thread did not start");
            method(pm,"beginPackageRemoval",apk).invoke(packages,owner);
        }
        insertion.join(5000);
        require(!insertion.isAlive()&&outcome.get() instanceof SecurityException&&entries(manager).isEmpty(),
                "Captured pre-lock account insertion survived provider retirement");
        require(method(svc,"authenticator",String.class).invoke(manager,"com.gesher.devsvc.account")==null,
                "Fresh authenticator capture accepted a retiring code generation");
        method(pm,"endPackageRemoval",apk).invoke(packages,owner);
        Account account=new Account("provider@example.test","fixture.type"),owned=new Account("owned@example.test","fixture.owned");
        Object provider=entry(entry,ownerName,ownerCode),clientOwned=entry(entry,clientName,clientCode);
        entries(manager).put(account,provider);entries(manager).put(owned,clientOwned);
        visibility(provider).put(clientKey,AccountManager.VISIBILITY_VISIBLE);
        visibility(provider).put(oldKey,AccountManager.VISIBILITY_VISIBLE);
        visibility(provider).put(otherKey,AccountManager.VISIBILITY_VISIBLE);
        grants(provider).put("scope",new HashSet<>(Arrays.asList(clientKey,oldKey,otherKey)));
        Class<?> cached=Class.forName(svc.getName()+"$CachedToken");
        Object cache=make(cached,new Class<?>[]{String.class,long.class},"synthetic-custom-token",Long.MAX_VALUE);
        custom(provider).put(clientKey+"\u0000scope",cache);custom(provider).put(oldKey+"\u0000scope",cache);
        custom(provider).put(otherKey+"\u0000scope",cache);
        method(svc,"beginMutation").invoke(manager);method(svc,"save").invoke(manager);
        try{method(pm,"checkPackageRemoval",String.class).invoke(packages,clientName);
            throw new AssertionError("Provider with active accounts passed preflight");}
        catch(InvocationTargetException expected){require(expected.getCause() instanceof UnsupportedOperationException,"Wrong provider preflight failure");}
        require(entries(manager).containsKey(owned)&&visibility(provider).containsKey(clientKey),
                "Provider preflight altered accounts or client grants");
        // Simulate the user removing that synthetic provider account before uninstall.
        method(svc,"beginMutation").invoke(manager);entries(manager).remove(owned);method(svc,"save").invoke(manager);
        require(persisted.get()!=null,"Transaction did not reach the server-local store seam");
        Object sessions=get(manager,"mSessions");
        Bundle failureSession=issue(sessions,clientKey,uid,ownerName,ownerCode);
        Properties beforeFailure=persisted.get();failWrite.set(true);
        try{method(pm,"revokePackageCapabilities",String.class).invoke(packages,clientName);
            throw new AssertionError("Injected failed write allowed uninstall capability cleanup");}
        catch(InvocationTargetException expected){require(expected.getCause() instanceof IllegalStateException,"Wrong persistence failure");}
        failWrite.set(false);
        require(persisted.get()==beforeFailure,"Failed persistence published a new serialized state");
        provider=entries(manager).get(account);
        require(entries(manager).size()==1&&visibility(provider).containsKey(clientKey)
                &&grants(provider).get("scope").contains(oldKey)&&custom(provider).containsKey(clientKey+"\u0000scope"),
                "Failed cleanup changed accounts or client grants");
        require((Boolean)method(svc,"callerUnchanged",caller).invoke(manager,oldCaller),"Failed cleanup advanced removal epoch");
        require(method(sessions.getClass(),"consume",Bundle.class,String.class,int.class,Predicate.class)
                .invoke(sessions,failureSession,clientKey,uid,(Predicate<Object>)p->true)!=null,"Failed cleanup retired a valid session");
        Bundle clientSession=issue(sessions,oldKey,uid,ownerName,ownerCode);
        Bundle ownerSession=issue(sessions,otherKey,ownerUid,clientName,clientCode);
        Bundle otherSession=issue(sessions,otherKey,ownerUid,ownerName,ownerCode);
        Object authenticator=make(auth,new Class<?>[]{String.class,ComponentName.class,int.class,int.class,int.class,int.class,boolean.class},
                "fixture.type",new ComponentName(ownerName,"FixtureAuthenticator"),0,0,0,0,false);
        Object pendingAnswer=make(answer,new Class<?>[]{svc,auth,caller},manager,authenticator,oldCaller);
        method(pm,"beginPackageRemoval",apk).invoke(packages,client);
        method(pm,"revokePackageCapabilities",String.class).invoke(packages,clientName);
        provider=entries(manager).get(account);
        require(provider!=null&&entries(manager).size()==1,"Client uninstall removed another provider's account");
        require(visibility(provider).size()==1&&visibility(provider).containsKey(otherKey),"Old signer visibility survived removal");
        require(grants(provider).get("scope").equals(Collections.singleton(otherKey)),"Client token grant survived or another grant was removed");
        require(custom(provider).size()==1&&custom(provider).containsKey(otherKey+"\u0000scope"),"Custom-token cleanup escaped exact package boundary");
        require("synthetic-provider-secret".equals(get(provider,"password")),"Other authenticator password changed");
        @SuppressWarnings("unchecked") Map<String,String> tokens=(Map<String,String>)get(provider,"tokens");
        require("synthetic-provider-token".equals(tokens.get("scope")),"Other provider token changed");
        deniedSession(sessions,clientSession,oldKey,uid);deniedSession(sessions,ownerSession,otherKey,ownerUid);
        require(method(sessions.getClass(),"consume",Bundle.class,String.class,int.class,Predicate.class)
                .invoke(sessions,otherSession,otherKey,ownerUid,(Predicate<Object>)p->true)!=null,"Unrelated opaque session was retired");
        require((Boolean)get(pendingAnswer,"complete")&&(Boolean)get(pendingAnswer,"failed"),"Interactive result capability survived removal");
        require(!(Boolean)method(svc,"callerUnchanged",caller).invoke(manager,oldCaller),"Old captured caller revived while original code remained installed");
        try{method(svc,"captureCaller",String.class,String.class,boolean.class,int.class,int.class)
                .invoke(manager,clientName,clientCode,false,uid,124);throw new AssertionError("New capture adopted removal epoch for old code");}
        catch(InvocationTargetException expected){require(expected.getCause() instanceof SecurityException,"Wrong retiring-caller failure");}
        method(pm,"endPackageRemoval",apk).invoke(packages,client);
        require(!(Boolean)method(svc,"callerUnchanged",caller).invoke(manager,oldCaller),"Clearing barrier revived an old captured caller");
        Object fresh=method(svc,"captureCaller",String.class,String.class,boolean.class,int.class,int.class)
                .invoke(manager,clientName,clientCode,false,uid,124);
        require((Boolean)method(svc,"callerUnchanged",caller).invoke(manager,fresh),"Fresh same-signer caller rejected after epoch retirement");
        Properties committed=persisted.get();
        require("1".equals(committed.getProperty("count")),"Serialized removal lost an unrelated provider account");
        require(!committed.containsValue(clientKey)&&!committed.containsValue(oldKey),"Serialized state retained old client grants");
        require(committed.containsValue("synthetic-provider-secret"),"Serialized removal lost unrelated provider secret");
        visibility(provider).put(clientKey,AccountManager.VISIBILITY_VISIBLE);
        require(((Account[])method(svc,"filterAccounts",String.class,caller).invoke(manager,null,oldCaller)).length==0,
                "Delayed pre-removal enumeration inherited a fresh same-signer visibility grant");
        require(((Account[])method(svc,"filterAccounts",String.class,caller).invoke(manager,null,fresh)).length==1,
                "Fresh caller could not enumerate its fresh grant");
        visibility(provider).remove(clientKey);
        // Owner predicate is deliberately reentrant: removal must win before final consumption.
        Bundle race=issue(sessions,clientKey,uid,ownerName,ownerCode);
        try{method(sessions.getClass(),"consume",Bundle.class,String.class,int.class,Predicate.class)
                .invoke(sessions,race,clientKey,uid,(Predicate<Object>)p->{
                    try{method(sessions.getClass(),"packageRemoved",String.class).invoke(sessions,clientName);}
                    catch(Exception e){throw new AssertionError(e);}return true;});
            throw new AssertionError("Session consumed after concurrent removal");}
        catch(InvocationTargetException expected){require(expected.getCause() instanceof SecurityException,"Wrong removal race failure");}
        method(pm,"beginPackageRemoval",apk).invoke(packages,owner);
        try{method(pm,"checkPackageRemoval",String.class).invoke(packages,ownerName);
            throw new AssertionError("Active provider was removed");}
        catch(InvocationTargetException expected){require(expected.getCause() instanceof UnsupportedOperationException,"Wrong active-provider failure");}
        require(method(svc,"validatedResult",auth,Bundle.class,Account.class)
                .invoke(manager,authenticator,new Bundle(),null)==null,"Authenticator result accepted during removal");
        method(pm,"endPackageRemoval",apk).invoke(packages,owner);
        require(entries(manager).containsKey(account)&&"synthetic-provider-secret".equals(get(provider,"password")),
                "Rejected provider removal lost an account or credential");
        System.out.println("AccountClientRemovalTest all passed: "+checks+" checks");
    }
}
