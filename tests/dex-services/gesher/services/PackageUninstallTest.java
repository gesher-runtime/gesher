/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.ActivityManager;
import android.app.PendingIntent;
import android.content.*;
import android.content.pm.*;
import android.content.res.AssetManager;
import android.os.*;
import com.gesher.server.ApkPackage;
import com.gesher.server.PackageManagerService;
import java.io.*;
import java.lang.reflect.*;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;
import java.util.function.*;

/** Synthetic request/consent, durability, storage recovery and permanent capability revocation. */
public final class PackageUninstallTest {
    private static int checks;
    private static void require(boolean value, String reason) { if (!value) throw new AssertionError(reason); checks++; }
    private static Method method(Class<?> c, String n, Class<?>... t) throws Exception {
        Method m=c.getDeclaredMethod(n,t);m.setAccessible(true);return m;
    }
    private static Field field(Class<?> c,String n) throws Exception { Field f=c.getDeclaredField(n);f.setAccessible(true);return f; }
    private static Object make(Class<?> c,Class<?>[] t,Object... a) throws Exception {
        Constructor<?> constructor=c.getDeclaredConstructor(t);constructor.setAccessible(true);return constructor.newInstance(a);
    }
    private interface Action { void run() throws Exception; }
    private static void rejected(Action action,Class<?> wanted) throws Exception {
        try { action.run(); } catch(Throwable failure) {
            if(failure instanceof InvocationTargetException)failure=failure.getCause();
            require(wanted.isInstance(failure),"Wrong failure: "+failure);return;
        }
        throw new AssertionError("Expected "+wanted.getSimpleName());
    }
    private static IntentSender receiver(AtomicReference<Intent> output) {
        return new IntentSender((IIntentSender)new IIntentSender.Stub() {
            @Override public void send(int code,Intent intent,String type,IBinder token,IIntentReceiver finished,
                    String permission,Bundle options) { output.set(intent); }
        });
    }
    private static void drain(Executor executor) throws Exception {
        CountDownLatch done=new CountDownLatch(1);executor.execute(done::countDown);
        require(done.await(5,TimeUnit.SECONDS),"Event queue failed to drain");
    }
    private static int status(AtomicReference<Intent> result) {
        require(result.get()!=null,"Missing final uninstall callback");
        return result.getAndSet(null).getIntExtra(PackageInstaller.EXTRA_STATUS,-99);
    }
    @SuppressWarnings("unchecked") public static void main(String[] args) throws Exception {
        require(args.length==3,"Pass target v1/v2 and independent ordinary caller APK");
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false,System.getenv("ANDROID_ROOT")+"/framework/framework-res.apk");
        File data=Files.createTempDirectory("gesher-uninstall-").toFile();
        android.system.Os.setenv("ANDROID_DATA",data.getPath(),true);
        android.system.Os.setenv("GESHER_MICROG_SPOOF","1",true);
        ApkPackage owner=ApkPackage.parse(args[2]);
        PackageManagerService pm=new PackageManagerService(Collections.singletonList(owner));
        Class<?> atmType=Class.forName("com.gesher.server.ActivityTaskManagerService");
        Object atm=make(atmType,new Class<?>[]{PackageManagerService.class},pm);
        Class<?> amType=Class.forName("com.gesher.server.ActivityManagerService");
        Class<?> notificationType=Class.forName("com.gesher.server.NotificationManagerService");
        Object notifications=make(notificationType,new Class<?>[]{PackageManagerService.class,amType,ToIntBiFunction.class},
                pm,null,(ToIntBiFunction<Object,Boolean>)(sbn,update)->0);
        Object am=make(amType,new Class<?>[]{atmType,PackageManagerService.class,notificationType},atm,pm,notifications);
        Class<?> installerType=Class.forName("com.gesher.server.Installer");
        Object installer=make(installerType,new Class<?>[]{PackageManagerService.class,atmType,Map.class},pm,atm,new LinkedHashMap<>());
        Method install=method(installerType,"install",String.class);
        install.invoke(installer,args[0]);
        String name=ApkPackage.parse(args[0]).packageName;
        require(!name.equals(owner.packageName),"Caller and target must differ");
        ApkPackage original=pm.get(name);int uid=original.uid();
        Object permission=pm.permissions();
        require(((Integer)method(permission.getClass(),"checkPermission",String.class,String.class,String.class,int.class)
                .invoke(permission,owner.packageName,"android.permission.DELETE_PACKAGES",null,0))==PackageManager.PERMISSION_DENIED,
                "Ordinary caller acquired privileged delete permission");
        List<Intent> events=Collections.synchronizedList(new ArrayList<>());
        Executor eventExecutor=(Executor)field(PackageManagerService.class,"mPackageEventExecutor").get(pm);
        drain(eventExecutor);
        AtomicReference<Throwable> eventFailure=new AtomicReference<>();
        field(PackageManagerService.class,"mPackageEventSender").set(pm,(Consumer<Intent>)event->{
            try {
                if(Intent.ACTION_PACKAGE_REMOVED.equals(event.getAction())||Intent.ACTION_PACKAGE_FULLY_REMOVED.equals(event.getAction())) {
                    require(!Thread.holdsLock(installer)&&!Thread.holdsLock(pm),"Removal callback holds publication lock");
                    require(pm.get(name)==null,"Removal event preceded PM removal");
                    require(!Files.readString(new File(data,"system/packages.list").toPath()).contains(name+"\t"),"Event preceded registry commit");
                    require(!new File(data,"data/"+name).exists(),"Old private data path survived publication");
                }
                events.add(new Intent(event));
            } catch(Throwable failure) { eventFailure.set(failure); }
        });
        Class<?> serviceType=Class.forName("com.gesher.server.PackageInstallerService");
        Class<?> consentType=Class.forName(serviceType.getName()+"$Consent");
        Class<?> removalType=Class.forName(serviceType.getName()+"$UninstallConsent");
        Class<?> transportType=Class.forName("com.gesher.server.InstallFileBridge$Transport");
        AtomicReference<String> caller=new AtomicReference<>(owner.packageName);
        AtomicBoolean allow=new AtomicBoolean(false);AtomicInteger prompts=new AtomicInteger();
        AtomicReference<Action> duringPrompt=new AtomicReference<>();List<Runnable> work=new ArrayList<>();
        Object consent=Proxy.newProxyInstance(consentType.getClassLoader(),new Class<?>[]{consentType},(p,m,a)->false);
        Object removal=Proxy.newProxyInstance(removalType.getClassLoader(),new Class<?>[]{removalType},(p,m,a)->{
            prompts.incrementAndGet();require(name.equals(a[1]),"Prompt targets another application");
            Action action=duringPrompt.getAndSet(null);if(action!=null)action.run();return allow.get();
        });
        IPackageInstaller service=(IPackageInstaller)make(serviceType,
                new Class<?>[]{PackageManagerService.class,Executor.class,consentType,Supplier.class,transportType,removalType},
                pm,(Executor)work::add,consent,(Supplier<String>)caller::get,null,removal);
        method(serviceType,"attach",atmType,installerType).invoke(service,atm,installer);
        AtomicReference<Intent> result=new AtomicReference<>();IntentSender receiver=receiver(result);
        VersionedPackage target=new VersionedPackage(name,-1L);
        rejected(()->service.uninstall(target,"foreign.owner",0,receiver,0),SecurityException.class);
        rejected(()->service.uninstall(target,owner.packageName,0,receiver,1),SecurityException.class);
        rejected(()->service.uninstall(null,owner.packageName,0,receiver,0),IllegalArgumentException.class);
        for(int flags:new int[]{1,2,4,8,0x1000}) {
            service.uninstall(target,owner.packageName,flags,receiver,0);
            require(status(result)!=PackageInstaller.STATUS_SUCCESS,"Unsupported flag changed removal authority");
        }
        service.uninstall(new VersionedPackage(name,99999L),owner.packageName,0,receiver,0);
        require(status(result)==PackageInstaller.STATUS_FAILURE_CONFLICT&&prompts.get()==0,"Stale version prompted or succeeded");
        service.uninstall(new VersionedPackage("android",-1L),owner.packageName,0,receiver,0);
        require(status(result)==PackageInstaller.STATUS_FAILURE_BLOCKED,"Platform package removal not blocked");
        service.uninstall(target,owner.packageName,0,receiver,0);
        require(work.size()==1&&pm.get(name)==original,"Uninstall ran before explicit host decision");
        service.uninstall(target,owner.packageName,0,receiver,0);
        require(status(result)==PackageInstaller.STATUS_FAILURE_BLOCKED&&work.size()==1,"Duplicate removal not bounded");
        work.remove(0).run();
        require(status(result)==PackageInstaller.STATUS_FAILURE_ABORTED&&pm.get(name)==original&&events.isEmpty(),"Default-No removed or notified");
        // The genuine native broker cannot show/approve a dialog from this non-server
        // fixture role. Native load/role failure must be a denial, never silent consent.
        IPackageInstaller deniedNative=(IPackageInstaller)make(serviceType,
                new Class<?>[]{PackageManagerService.class},pm);
        method(serviceType,"attach",atmType,installerType).invoke(deniedNative,atm,installer);
        deniedNative.uninstall(target,owner.packageName,0,receiver,0);
        for(int attempt=0;attempt<100&&result.get()==null;attempt++)Thread.sleep(25);
        require(status(result)==PackageInstaller.STATUS_FAILURE_ABORTED&&pm.get(name)==original,
                "Unavailable real host consent broker removed target");
        allow.set(true);
        // Replacing the caller object, even with identical signed bytes, invalidates consent.
        Method add=method(PackageManagerService.class,"add",ApkPackage.class);
        duringPrompt.set(()->add.invoke(pm,ApkPackage.parse(args[2])));
        service.uninstall(target,owner.packageName,0,receiver,0);work.remove(0).run();
        require(status(result)==PackageInstaller.STATUS_FAILURE_BLOCKED&&pm.get(name)==original,"Stale caller consent removed target");
        duringPrompt.set(()->install.invoke(installer,args[1]));
        service.uninstall(target,owner.packageName,0,receiver,0);work.remove(0).run();
        drain(eventExecutor);events.clear();
        require(status(result)==PackageInstaller.STATUS_FAILURE_BLOCKED&&pm.get(name).versionCode()==2,"Stale target consent removed update");

        Class<?> processType=Class.forName(atmType.getName()+"$ProcessRecord");
        Object process=make(processType,new Class<?>[]{atmType,int.class,android.app.IApplicationThread.class},atm,12345,null);
        field(processType,"packageName").set(process,owner.packageName);
        List<Object> processes=(List<Object>)field(atmType,"mProcesses").get(atm);processes.add(process);
        Method currentCaller=method(serviceType,"removalCallerCurrent",String.class,ApkPackage.class,int.class,boolean.class,processType);
        require((Boolean)currentCaller.invoke(service,owner.packageName,pm.get(owner.packageName),12345,false,process),"Live original caller not recognized");
        processes.remove(process);
        Object reused=make(processType,new Class<?>[]{atmType,int.class,android.app.IApplicationThread.class},atm,12345,null);
        field(processType,"packageName").set(reused,owner.packageName);processes.add(reused);
        require(!(Boolean)currentCaller.invoke(service,owner.packageName,pm.get(owner.packageName),12345,false,process),"Reused PID inherited earlier removal consent");
        processes.clear();

        File privateData=new File(data,"data/"+name);privateData.mkdirs();
        Files.writeString(new File(privateData,"private-marker").toPath(),"synthetic old data");
        Method pendingRemoval=method(PackageManagerService.class,"isRemovalPending",String.class);
        AtomicBoolean preflightBlocked=new AtomicBoolean(true);AtomicInteger cleanups=new AtomicInteger();
        method(PackageManagerService.class,"onPackageRemovalCheck",Consumer.class).invoke(pm,(Consumer<String>)removed->{
            if(preflightBlocked.get())throw new SecurityException("Synthetic provider still owns accounts");
        });
        ComponentName replacement=new ComponentName(name,pm.getPackageInfo(name,PackageManager.GET_RECEIVERS,0).receivers[0].name);
        require(pm.getReceiverInfo(replacement,0,0)!=null,"Target receiver unavailable before retirement");
        method(PackageManagerService.class,"onPackageRemoval",Consumer.class).invoke(pm,(Consumer<String>)removed->{
            try {
                cleanups.incrementAndGet();
                require((Boolean)pendingRemoval.invoke(pm,name)&&pm.get(name)!=null,"Cleanup did not hold original-code retirement barrier");
                require(pm.getReceiverInfo(replacement,0,0)==null,"Retiring component remained dispatchable");
                rejected(()->install.invoke(installer,args[1]),IllegalStateException.class);
            } catch(Exception failure) { throw new IllegalStateException(failure); }
        });
        service.uninstall(target,caller.get(),0,receiver,0);work.remove(0).run();
        require(status(result)==PackageInstaller.STATUS_FAILURE_BLOCKED&&cleanups.get()==0
                &&new File(privateData,"private-marker").isFile(),"Rejected preflight mutated client/data state");
        preflightBlocked.set(false);
        // A real write failure must preserve PM, registry and old app data; no removal event.
        File temporary=new File(data,"system/packages.list.tmp");temporary.mkdirs();
        ApkPackage beforeFailure=pm.get(name);
        service.uninstall(target,caller.get(),0,receiver,0);work.remove(0).run();
        require(status(result)==PackageInstaller.STATUS_FAILURE_STORAGE,"Registry failure reported success");
        require(pm.get(name)==beforeFailure&&new File(privateData,"private-marker").isFile(),"Failed removal lost publication/data");
        require(Files.readString(new File(data,"system/packages.list").toPath()).contains(name+"\t")&&events.isEmpty(),"Failed removal changed registry/events");
        require(!(Boolean)pendingRemoval.invoke(pm,name),"Successful rollback retained removal barrier");
        Files.delete(temporary.toPath());

        Object pending=method(amType,"pendingIntents").invoke(am);Class<?> pendingType=pending.getClass();
        Object held=method(pendingType,"get",int.class,String.class,int.class,Intent[].class,String[].class,int.class)
                .invoke(pending,ActivityManager.INTENT_SENDER_BROADCAST,name,7,new Intent[]{new Intent("fixture.old")},null,PendingIntent.FLAG_IMMUTABLE);
        require(held!=null,"Old pending-intent capability was not created");
        CountDownLatch beforePendingInsert=new CountDownLatch(1),releasePending=new CountDownLatch(1);
        AtomicReference<Throwable> pendingFailure=new AtomicReference<>();
        field(pendingType,"mBeforeRecordLock").set(pending,(Runnable)()->{
            beforePendingInsert.countDown();
            try { if(!releasePending.await(15,TimeUnit.SECONDS))throw new AssertionError("Pending insert race timed out"); }
            catch(InterruptedException interrupted) { throw new AssertionError(interrupted); }
        });
        Thread pendingRace=new Thread(()->{
            try {
                method(pendingType,"get",int.class,String.class,int.class,Intent[].class,String[].class,int.class)
                    .invoke(pending,ActivityManager.INTENT_SENDER_BROADCAST,name,8,new Intent[]{new Intent("fixture.paused")},null,PendingIntent.FLAG_IMMUTABLE);
            } catch(Throwable failure) { pendingFailure.set(failure instanceof InvocationTargetException?failure.getCause():failure); }
        });pendingRace.setDaemon(true);pendingRace.start();
        require(beforePendingInsert.await(5,TimeUnit.SECONDS),"Pending intent did not pause before publication");
        field(pendingType,"mBeforeRecordLock").set(pending,null);
        Object uris=method(amType,"uriGrants").invoke(am);Class<?> grantType=Class.forName("com.gesher.server.UriGrants$Grant");
        List<Object> grants=(List<Object>)field(uris.getClass(),"mGrants").get(uris);
        grants.add(make(grantType,new Class<?>[]{ApkPackage.class,ApkPackage.class,android.net.Uri.class,int.class,boolean.class},
                pm.get(name),owner,android.net.Uri.parse("content://fixture/item"),1,false));
        Class<?> jobsType=Class.forName("com.gesher.server.JobSchedulerService");
        Object scheduler=make(jobsType,new Class<?>[]{amType,atmType,PackageManagerService.class},am,atm,pm);
        Class<?> jobType=Class.forName(jobsType.getName()+"$Job");
        android.app.job.JobInfo jobInfo=new android.app.job.JobInfo.Builder(7,new ComponentName(name,"fixture.Job"))
                .setMinimumLatency(100000).setPersisted(true).build();
        Object job=make(jobType,new Class<?>[]{jobsType,String.class,String.class,android.app.job.JobInfo.class},scheduler,name,null,jobInfo);
        List<Object> jobs=(List<Object>)field(jobsType,"mJobs").get(scheduler);jobs.add(job);
        method(jobsType,"persist",jobType).invoke(scheduler,job);
        File persisted=(File)method(jobsType,"persistFile",jobType).invoke(null,job);
        require(persisted.isFile(),"Persisted synthetic job not written");
        CountDownLatch beforePersist=new CountDownLatch(1),releasePersist=new CountDownLatch(1);
        AtomicReference<Throwable> persistFailure=new AtomicReference<>();
        field(jobsType,"mBeforePersist").set(scheduler,(Runnable)()->{
            beforePersist.countDown();
            try { if(!releasePersist.await(15,TimeUnit.SECONDS))throw new AssertionError("Job persistence race timed out"); }
            catch(InterruptedException interrupted) { throw new AssertionError(interrupted); }
        });
        Thread persistRace=new Thread(()->{
            try { method(jobsType,"persist",jobType).invoke(scheduler,job); }
            catch(Throwable failure) { persistFailure.set(failure); }
        });persistRace.setDaemon(true);persistRace.start();
        require(beforePersist.await(5,TimeUnit.SECONDS),"Job did not pause before persistence");
        field(jobsType,"mBeforePersist").set(scheduler,null);
        PackageInstaller.SessionParams params=new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        caller.set(name);int session=service.createSession(params,name,null,0);IPackageInstallerSession oldSession=service.openSession(session);caller.set(owner.packageName);
        service.uninstall(new VersionedPackage(name,2L),caller.get(),0,receiver,0);work.remove(0).run();
        require(status(result)==PackageInstaller.STATUS_SUCCESS,"Approved ordinary target was not removed");
        drain(eventExecutor);require(eventFailure.get()==null,"Removal callback failed: "+eventFailure.get());
        require(pm.get(name)==null&&!privateData.exists(),"Removed package still visible or has reusable data");
        require(!(Boolean)pendingRemoval.invoke(pm,name),"Committed removal retained barrier");
        require(grants.isEmpty()&&jobs.isEmpty()&&!persisted.exists(),"URI or persisted job capability survived removal");
        require(events.size()==2&&Intent.ACTION_PACKAGE_REMOVED.equals(events.get(0).getAction())
                &&Intent.ACTION_PACKAGE_FULLY_REMOVED.equals(events.get(1).getAction()),"Missing ordered removal events");
        for(Intent event:events) require(("package:"+name).equals(event.getDataString())
                &&event.getIntExtra(Intent.EXTRA_UID,-1)==uid&&event.getBooleanExtra(Intent.EXTRA_DATA_REMOVED,false)
                &&!event.getBooleanExtra(Intent.EXTRA_REPLACING,true),"Removal event fabricates URI/UID/data semantics");
        install.invoke(installer,args[1]);drain(eventExecutor);
        releasePending.countDown();releasePersist.countDown();pendingRace.join(5000);persistRace.join(5000);
        require(!pendingRace.isAlive()&&pendingFailure.get() instanceof SecurityException,
                "Pre-uninstall pending insertion published after same-signer reinstall");
        require(!persistRace.isAlive()&&persistFailure.get()==null&&!persisted.exists(),
                "Pre-uninstall paused persistence recreated a removed job file");
        require(pm.get(name).uid()==uid&&!new File(privateData,"private-marker").exists(),"Reinstall reused old data or changed reserved UID");
        require((Boolean)field(held.getClass(),"canceled").get(held),"Old pending-intent capability revived");
        require(jobs.isEmpty()&&!persisted.exists()&&grants.isEmpty(),"Same-signer reinstall revived job/URI state");
        caller.set(name);rejected(oldSession::getNames,SecurityException.class);caller.set(owner.packageName);
        require(((Integer)method(permission.getClass(),"checkPermission",String.class,String.class,String.class,int.class)
                .invoke(permission,owner.packageName,"android.permission.DELETE_PACKAGES",null,0))==PackageManager.PERMISSION_DENIED,
                "Host approval fabricated persistent delete permission");

        // Journal recovery restores all precommit moves and reaps only after durable removal.
        Class<?> storage=Class.forName("com.gesher.server.PackageRemovalStorage");
        ApkPackage reinstalled=pm.get(name);String code=new File(reinstalled.apkPath).getParent();
        privateData.mkdirs();Files.writeString(new File(privateData,"recovery-marker").toPath(),"synthetic recovery");
        Object staged=make(storage,new Class<?>[]{String.class,String.class},name,code);
        method(storage,"stage").invoke(staged);require(!privateData.exists(),"Data quarantine did not fence reinstall");
        Map<String,String> retained=new LinkedHashMap<>();retained.put(name,code);
        method(storage,"recover",Map.class).invoke(null,retained);
        require(new File(privateData,"recovery-marker").isFile()&&new File(reinstalled.apkPath).isFile(),"Precommit recovery failed");
        rejected(()->make(storage,new Class<?>[]{String.class,String.class},name,new File(data,"outside").getPath()),SecurityException.class);

        Object inDoubt=make(storage,new Class<?>[]{String.class,String.class},name,code);
        method(PackageManagerService.class,"beginPackageRemoval",ApkPackage.class).invoke(pm,reinstalled);
        method(storage,"stage").invoke(inDoubt);
        Files.createDirectory(new File(code).toPath()); // Deliberately obstruct rollback.
        rejected(()->method(storage,"rollback").invoke(inDoubt),IOException.class);
        rejected(()->install.invoke(installer,args[1]),IOException.class);
        require(pm.get(name)==reinstalled&&(Boolean)pendingRemoval.invoke(pm,name),"In-doubt removal allowed publication or new captures");
        Files.delete(new File(code).toPath());
        install.invoke(installer,args[1]);
        require(!(Boolean)pendingRemoval.invoke(pm,name),"Recovered rollback did not release barrier");
        require(new File(privateData,"recovery-marker").isFile(),"Unblocked rollback lost original data");

        // Simulate committed removal whose private remnants could not be reaped.
        Object committed=make(storage,new Class<?>[]{String.class,String.class},name,code);
        method(storage,"stage").invoke(committed);
        ((Map<?,?>)field(installerType,"mEntries").get(installer)).remove(name);
        method(installerType,"writeRegistry").invoke(installer);
        method(PackageManagerService.class,"remove",String.class).invoke(pm,name);
        Field reaper=field(storage,"sReaper");Object genuineReaper=reaper.get(null);
        String fresh;
        try {
            reaper.set(null,(Predicate<Path>)path->false); // Simulate locked committed remnants.
            require((Boolean)method(storage,"reserves",Path.class).invoke(null,new File(code).toPath()),
                    "Surviving removal journal does not reserve its original generation");
            install.invoke(installer,args[1]);
            fresh=new File(pm.get(name).apkPath).getParent();
            require(!fresh.equals(code),"Fresh reinstall reused a quarantined code generation");
            privateData.mkdirs();Files.writeString(new File(privateData,"fresh-marker").toPath(),"fresh synthetic data");
        } finally { reaper.set(null,genuineReaper); }
        Map<String,String> after=new LinkedHashMap<>();after.put(name,fresh);
        method(storage,"recover",Map.class).invoke(null,after);
        require(!new File(privateData,"recovery-marker").exists()&&new File(pm.get(name).apkPath).isFile()
                &&new File(privateData,"fresh-marker").isFile(),
                "Committed old data returned after reinstall/recovery");

        // Original stores can reflect PackageManager.deletePackage instead of using
        // PackageInstaller. Exercise the actual PM entry point and legacy observer,
        // sharing the same synthetic default-No/fresh-consent worker as the modern API.
        field(PackageManagerService.class,"mInstallerService").set(pm,service);
        AtomicInteger legacyResult=new AtomicInteger(-999),legacyCallbacks=new AtomicInteger();
        IPackageDeleteObserver legacyObserver=new IPackageDeleteObserver.Stub() {
            @Override public void packageDeleted(String removed,int code) {
                require(name.equals(removed),"Legacy observer named a different target");
                legacyResult.set(code);legacyCallbacks.incrementAndGet();
            }
        };
        int beforePrompts=prompts.get();
        pm.deletePackageAsUser(name,-1,legacyObserver,0,4);
        require(legacyResult.get()==-1&&prompts.get()==beforePrompts&&pm.get(name)!=null,
                "Unsupported legacy flag bypassed policy or omitted callback");
        rejected(()->pm.deletePackageAsUser(name,-1,legacyObserver,1,0),SecurityException.class);
        pm.deletePackageAsUser(name,99999,legacyObserver,0,0);
        require(legacyResult.get()==-1&&prompts.get()==beforePrompts&&pm.get(name)!=null,
                "Legacy stale version prompted or removed target");
        allow.set(false);legacyResult.set(-999);
        ApkPackage beforeLegacy=pm.get(name);
        pm.deletePackageAsUser(name,-1,legacyObserver,0,0);
        require(work.size()==1&&prompts.get()==beforePrompts&&legacyResult.get()==-999,
                "Legacy removal ran before fresh host consent");
        work.remove(0).run();
        require(legacyResult.get()==-5&&pm.get(name)==beforeLegacy&&prompts.get()==beforePrompts+1,
                "Legacy default-No removed target or lost cancellation callback");
        allow.set(true);legacyResult.set(-999);
        pm.deletePackageAsUser(name,-1,legacyObserver,0,0);work.remove(0).run();
        require(legacyResult.get()==1&&pm.get(name)==null&&prompts.get()==beforePrompts+2,
                "Approved legacy removal did not complete through shared pipeline");
        require(!privateData.exists()&&legacyCallbacks.get()==4,
                "Legacy removal retained app data or miscounted final callbacks");
        require(((Integer)method(permission.getClass(),"checkPermission",String.class,String.class,String.class,int.class)
                .invoke(permission,owner.packageName,"android.permission.DELETE_PACKAGES",null,0))==PackageManager.PERMISSION_DENIED,
                "Legacy adapter granted privileged deletion permission");
        System.out.println("PackageUninstallTest all passed: "+checks+" checks");
    }
}
