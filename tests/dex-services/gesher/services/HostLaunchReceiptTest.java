/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.*;
import android.content.*;
import android.content.pm.ActivityInfo;
import android.content.res.AssetManager;
import android.os.*;
import com.gesher.server.*;
import java.lang.reflect.*;
import java.nio.file.Files;
import java.util.*;
import java.util.concurrent.atomic.*;

/** Retained generation and actual host-start routing; no app code or native window is run. */
public final class HostLaunchReceiptTest {
    private static int checks;
    private static void require(boolean okay, String why) { if (!okay) throw new AssertionError(why); checks++; }
    private static Field field(Class<?> c, String n) throws Exception { Field f=c.getDeclaredField(n);f.setAccessible(true);return f; }
    private static Object get(Object o, String n) throws Exception { return field(o.getClass(),n).get(o); }
    private static void set(Object o, String n, Object v) throws Exception { field(o.getClass(),n).set(o,v); }
    private static Method method(Class<?> c,String n,Class<?>...a)throws Exception { Method m=c.getDeclaredMethod(n,a);m.setAccessible(true);return m; }
    private static Object make(Class<?> c,Class<?>[] a,Object...v)throws Exception { Constructor<?> k=c.getDeclaredConstructor(a);k.setAccessible(true);return k.newInstance(v); }
    private static void generations() {
        ProcessExitStatus status=new ProcessExitStatus(); String key="fixture\nfixture:used";
        require(status.snapshot(null).state==ProcessExitStatus.State.UNAVAILABLE,"Missing receipt invented a process");
        status.started(key,1); ProcessExitStatus.Generation old=status.capture(key,1);
        require(old!=null && status.snapshot(old).state==ProcessExitStatus.State.STARTING,"Unbound receipt not starting");
        require(!status.exited(old,100,0) && !status.snapshot(old).hasExitCode,"Unbound receipt accepted an exit");
        require(status.bind(key,1,100) && status.snapshot(old).state==ProcessExitStatus.State.RUNNING,"Bound generation not running");
        status.started(key,2); ProcessExitStatus.Generation latest=status.capture(key,2);status.bind(key,2,101);
        require(!status.exited(old,101,0),"Wrong PID filled an old receipt");
        require(status.exited(old,100,23) && status.snapshot(old).exitCode==23,"Old retained generation lost its real exit");
        require(status.code(key)==null && !status.snapshot(latest).hasExitCode,"Old receipt overwrote the latest-name result");
        require(status.exited(key,2,101,0) && status.code(key)==0 && status.snapshot(old).exitCode==23,
                "New success masked the requested generation's failure");
        require(!status.exited(old,100,0) && status.snapshot(old).exitCode==23,"Conflicting exit replaced failure");
        status.started("sibling",3);status.bind("sibling",3,102);status.exited("sibling",3,102,17);
        require(status.snapshot(old).exitCode==23 && status.code(key)==0,"Sibling result replaced launch outcome");
        ProcessExitStatus foreign=new ProcessExitStatus();foreign.started(key,4);foreign.bind(key,4,103);
        require(!status.exited(foreign.capture(key,4),103,0),"Foreign status object accepted a generation");
        status.started(key,5);ProcessExitStatus.Generation canceled=status.capture(key,5);status.unavailable(canceled);
        require(status.snapshot(canceled).state==ProcessExitStatus.State.UNAVAILABLE && !status.snapshot(canceled).hasExitCode,
                "Failed pre-PID start invented success or waited forever");
        status.started(key,6);ProcessExitStatus.Generation minimum=status.capture(key,6);status.bind(key,6,100);
        status.exited(minimum,100,Integer.MIN_VALUE);
        require(status.snapshot(minimum).state==ProcessExitStatus.State.FINISHED
                && status.snapshot(minimum).hasExitCode && status.snapshot(minimum).exitCode==Integer.MIN_VALUE,
                "A real MIN_VALUE exit was confused with unknown");
        require(!status.exited(key,5,100,0) && status.snapshot(minimum).exitCode==Integer.MIN_VALUE,
                "PID reuse bypassed generation fencing");
        require(status.bind(canceled,104) && status.exited(canceled,104,17)
                && status.snapshot(canceled).hasExitCode && status.snapshot(canceled).exitCode==17
                && status.snapshot(minimum).exitCode==Integer.MIN_VALUE,
                "A genuinely observed late canceled-start exit was lost or replaced a newer generation");
    }
    private static int[] receipt(IBinder binder) throws Exception {
        Parcel data=Parcel.obtain(),reply=Parcel.obtain();
        try {
            data.writeInterfaceToken("com.gesher.ILaunchExit");data.setDataPosition(0);
            method(binder.getClass(),"onTransact",int.class,Parcel.class,Parcel.class,int.class)
                    .invoke(binder,IBinder.FIRST_CALL_TRANSACTION,data,reply,0);
            reply.setDataPosition(0);reply.readException();
            return new int[]{reply.readInt(),reply.readBoolean()?1:0,reply.readInt()};
        } finally {data.recycle();reply.recycle();}
    }
    private static final class AppThread extends IApplicationThread.Default {
        final Binder binder=new Binder();@Override public IBinder asBinder(){return binder;}
    }
    @SuppressWarnings({"unchecked","rawtypes"})
    private static void launchReadiness(Object tasks, Class<?> atm, Class<?> recordType,
            ActivityInfo info) throws Exception {
        Class<?>[] signature = {atm, ActivityInfo.class, Intent.class, int.class,
                int.class, String.class, recordType, String.class, int.class};
        Intent intent = new Intent().setComponent(new ComponentName(info.packageName, info.name));
        Object older = make(recordType, signature, tasks, info, new Intent(intent),
                700, Binder.getCallingPid(), "com.gesher.launcher", null, null, -1);
        Object pending = make(recordType, signature, tasks, info, new Intent(intent),
                700, Binder.getCallingPid(), "com.gesher.launcher", null, null, -1);
        Object newer = make(recordType, signature, tasks, info, new Intent(intent),
                700, Binder.getCallingPid(), "com.gesher.launcher", null, null, -1);
        Object foreign = make(recordType, signature, tasks, info, new Intent(intent),
                701, Binder.getCallingPid(), "com.gesher.launcher", null, null, -1);
        Class<?> state = Class.forName(atm.getName()+"$State");
        for (Object record : new Object[]{older,pending,newer,foreign}) {
            // Real activities can share a coarse Windows clock tick.
            set(record,"startTime",100L);
            set(record,"state",Enum.valueOf((Class)state,"RESUMED"));
        }
        set(older,"idle",true);set(foreign,"idle",true);
        List<Object> records=(List<Object>)get(tasks,"mActivities");
        Method above=method(atm,"idleAbove",recordType);
        Method waitForStart=method(atm,"waitForStarted",int.class,recordType);
        synchronized(tasks) {
            records.addAll(Arrays.asList(older,pending,newer,foreign));
            try {
                require(above.invoke(tasks,pending)==null,
                        "Older same-tick or foreign-task idle activity completed a new launch");
                set(newer,"idle",true);
                require(above.invoke(tasks,pending)==newer,
                        "A later idle activity in the same task did not complete its trampoline");
                set(newer,"state",Enum.valueOf((Class)state,"DESTROYED"));
                require(above.invoke(tasks,pending)==null,"Destroyed successor completed a launch");
                records.remove(pending);
                set(pending,"state",Enum.valueOf((Class)state,"DESTROYED"));
                set(pending,"launchFailed",true);
                set(newer,"state",Enum.valueOf((Class)state,"RESUMED"));
                require(above.invoke(tasks,pending)==newer,
                        "Removing the original record lost the later task activity's order");
                WaitResult ready=(WaitResult)waitForStart.invoke(tasks,ActivityManager.START_SUCCESS,pending);
                require(ready.result==ActivityManager.START_SUCCESS && !ready.timeout,
                        "An already-idle successor was replaced by the original process failure");
                set(newer,"state",Enum.valueOf((Class)state,"DESTROYED"));
                WaitResult failed=(WaitResult)waitForStart.invoke(tasks,ActivityManager.START_SUCCESS,pending);
                require(failed.result==ActivityManager.START_CANCELED && !failed.timeout,
                        "Failed pre-idle launch was reported as a successful start");
                set(pending,"launchFailed",false);set(pending,"finishing",true);
                WaitResult finished=(WaitResult)waitForStart.invoke(tasks,ActivityManager.START_SUCCESS,pending);
                require(finished.result==ActivityManager.START_SUCCESS && !finished.timeout,
                        "Deliberate finish-onCreate changed the existing start result");
                set(pending,"launchFailed",true);set(pending,"idle",true);
                WaitResult wasReady=(WaitResult)waitForStart.invoke(tasks,ActivityManager.START_SUCCESS,pending);
                require(wasReady.result==ActivityManager.START_SUCCESS,
                        "A completed activity's earlier idle report was discarded");
            } finally { records.removeAll(Arrays.asList(older,pending,newer,foreign)); }
        }
    }
    @SuppressWarnings({"unchecked","rawtypes"})
    public static void main(String[] args)throws Exception {
        require(args.length==1,"Pass the synthetic webviewlifecycle APK");
        System.loadLibrary("android_runtime");generations();
        AssetManager.createSystemAssetsInZygoteLocked(false,System.getenv("ANDROID_ROOT")+"/framework/framework-res.apk");
        android.system.Os.setenv("ANDROID_DATA",Files.createTempDirectory("host-launch-receipt-").toString(),true);
        android.system.Os.setenv("GESHER_MICROG_SPOOF","1",true);
        Class<?> apk=Class.forName("com.gesher.server.ApkPackage"),pm=Class.forName("com.gesher.server.PackageManagerService");
        Object code=apk.getMethod("parse",String.class).invoke(null,args[0]);String pkg=(String)apk.getField("packageName").get(code);
        Object packages=pm.getConstructor(Collection.class).newInstance(Collections.singletonList(code));
        Class<?> atm=Class.forName("com.gesher.server.ActivityTaskManagerService"),am=Class.forName("com.gesher.server.ActivityManagerService");
        Class<?> notifications=Class.forName("com.gesher.server.NotificationManagerService");
        Object tasks=make(atm,new Class<?>[]{pm},packages);
        make(am,new Class<?>[]{atm,pm,notifications},tasks,packages,make(notifications,new Class<?>[0]));
        ProcessExitStatus status=(ProcessExitStatus)get(tasks,"mExitStatus");
        Class<?> starter=Class.forName(atm.getName()+"$ProcessStarter");
        AtomicInteger starts=new AtomicInteger();AtomicReference<ProcessExitStatus.Generation> captured=new AtomicReference<>();
        AtomicInteger taskId=new AtomicInteger();
        Object start=Proxy.newProxyInstance(starter.getClassLoader(),new Class<?>[]{starter},(p,m,a)->{
            int round=starts.incrementAndGet();long seq=(Long)a[2];
            require(pkg.equals(a[0]) && (pkg+":used").equals(a[1]),"Host start trusted claimed process extras");
            Object pending=((Map<?,?>)get(tasks,"mStarting")).get(seq);
            ProcessExitStatus.Generation generation=(ProcessExitStatus.Generation)get(pending,"exitGeneration");captured.set(generation);
            List<?> records=(List<?>)get(tasks,"mActivities");Object record=records.get(records.size()-1);
            require(get(record,"launchGeneration")==generation,"Generation was not captured before native creation");
            taskId.set((Integer)get(record,"taskId"));
            if(round==2) {
                String key=(String)method(atm,"processKey",String.class,String.class).invoke(null,pkg,pkg+":used");
                require(status.bind(key,seq,77301) && status.exited(generation,77301,23),"Synthetic OS result failed");
                status.started(key,seq+100);status.bind(key,seq+100,77302);status.exited(key,seq+100,77302,0);
            }
            if(round==3) {
                method(atm,"forceStopPackage",String.class).invoke(tasks,pkg);
                require(status.snapshot(generation).state==ProcessExitStatus.State.UNAVAILABLE,
                        "Pre-PID cancellation left a starting receipt live");
                return 0; // No native process is ever created by this fixture.
            }
            throw new IllegalStateException("Synthetic starter stops before executing app code");
        });
        method(atm,"setProcessStarter",starter).invoke(tasks,start);
        Method launch=method(atm,"startActivityForHost",Intent.class);
        Intent intent=new Intent().setComponent(new ComponentName(pkg,pkg+".UsedActivity"))
                .putExtra("processName",pkg+":sibling").putExtra("startSeq",999L).putExtra("taskId",-99);
        GesherService.LaunchResult failed=(GesherService.LaunchResult)launch.invoke(tasks,intent);
        int[] unavailable=receipt(failed.exitReceipt);
        require(failed.taskId==taskId.get() && failed.result.who.equals(intent.getComponent()),"Task/component receipt was guessed");
        require(unavailable[0]==3 && unavailable[1]==0,"Failed pre-attach creation invented an exit code");
        require(failed.result.result==ActivityManager.START_CANCELED && !failed.result.timeout,
                "Failed native creation was reported as a successful detached start");
        GesherService.LaunchResult fast=(GesherService.LaunchResult)launch.invoke(tasks,intent);
        int[] exit=receipt(fast.exitReceipt);
        require(exit[0]==2 && exit[1]==1 && exit[2]==23,"Fast requested exit was replaced by a newer success");
        require(fast.taskId==taskId.get(),"Receipt selected a sibling task");
        require(fast.result.result==ActivityManager.START_CANCELED,
                "Fast pre-attach failure was reported as a successful detached start");
        GesherService.LaunchResult missing=(GesherService.LaunchResult)launch.invoke(tasks,
                new Intent().setComponent(new ComponentName(pkg,pkg+".MissingActivity")));
        require(missing.taskId==-1 && receipt(missing.exitReceipt)[0]==3 && starts.get()==2,"Unresolved launch invented authority");
        GesherService.LaunchResult canceled=(GesherService.LaunchResult)launch.invoke(tasks,intent);
        require(receipt(canceled.exitReceipt)[0]==3 && receipt(canceled.exitReceipt)[1]==0
                && ((Map<?,?>)get(tasks,"mStarting")).isEmpty(),"Canceled/invalid native start invented a result or leaked its reservation");

        // The standard reused/delivered-to-top path must retain the actual record too.
        ActivityInfo info=(ActivityInfo)pm.getMethod("getActivityInfo",ComponentName.class,long.class,int.class)
                .invoke(packages,intent.getComponent(),0L,0);
        Class<?> recordType=Class.forName(atm.getName()+"$ActivityRecord"),processType=Class.forName(atm.getName()+"$ProcessRecord");
        Object process=make(processType,new Class<?>[]{atm,int.class,IApplicationThread.class},tasks,77303,new AppThread());
        set(process,"packageName",pkg);set(process,"processName",pkg+":used");set(process,"ready",true);
        Object record=make(recordType,new Class<?>[]{atm,ActivityInfo.class,Intent.class,int.class,int.class,String.class,recordType,String.class,int.class},
                tasks,info,new Intent(intent),37,Binder.getCallingPid(),"com.gesher.launcher",null,null,-1);
        set(record,"app",process);set(record,"idle",true);
        Class<?> state=Class.forName(atm.getName()+"$State");set(record,"state",Enum.valueOf((Class)state,"RESUMED"));
        set(record,"launchGeneration",captured.get());
        ((List<Object>)get(tasks,"mProcesses")).add(process);((List<Object>)get(tasks,"mActivities")).add(record);
        ((List<Object>)get(process,"activities")).add(record);
        ActivityOptions options=ActivityOptions.makeBasic();options.setLaunchTaskId(37);
        WaitResult reused=((IActivityTaskManager)tasks).startActivityAndWait(null,"com.gesher.launcher",null,
                new Intent(intent).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),null,null,null,0,0,null,options.toBundle(),0);
        require(reused.result==ActivityManager.START_DELIVERED_TO_TOP && reused.who.equals(intent.getComponent())
                && get(record,"launchGeneration")==captured.get() && starts.get()==3,"Reused activity lost its actual record/generation");
        launchReadiness(tasks,atm,recordType,info);
        System.out.println("HostLaunchReceiptTest all passed: "+checks+" generation, exact route, fast exit, reuse and launch readiness checks");
    }
}
