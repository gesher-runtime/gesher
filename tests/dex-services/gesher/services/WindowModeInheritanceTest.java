/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.IActivityTaskManager;
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
import java.util.concurrent.atomic.*;

/** Real activity-start routing with a server-local starter; no application code or window runs. */
public final class WindowModeInheritanceTest {
    private static int checks;
    private static void require(boolean value,String reason){if(!value)throw new AssertionError(reason);checks++;}
    private static Field field(Class<?> type,String name)throws Exception{Field f=type.getDeclaredField(name);f.setAccessible(true);return f;}
    private static Object get(Object o,String name)throws Exception{return field(o.getClass(),name).get(o);}
    private static void set(Object o,String name,Object value)throws Exception{field(o.getClass(),name).set(o,value);}
    private static Method method(Class<?> type,String name,Class<?>...args)throws Exception{Method m=type.getDeclaredMethod(name,args);m.setAccessible(true);return m;}
    private static Object make(Class<?> type,Class<?>[] args,Object...values)throws Exception{Constructor<?> c=type.getDeclaredConstructor(args);c.setAccessible(true);return c.newInstance(values);}
    private static final class ThreadStub extends IApplicationThread.Default{
        final Binder binder=new Binder(); @Override public IBinder asBinder(){return binder;}
    }
    @SuppressWarnings({"unchecked","rawtypes"})
    public static void main(String[] args)throws Exception{
        require(args.length==2,"Pass target and source fixture APKs");
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false,System.getenv("ANDROID_ROOT")+"/framework/framework-res.apk");
        android.system.Os.setenv("ANDROID_DATA",Files.createTempDirectory("window-policy-").toString(),true);
        android.system.Os.setenv("GESHER_MICROG_SPOOF","1",true);
        android.system.Os.setenv("GESHER_APP_WINDOW","headless",true);
        Class<?> apk=Class.forName("com.gesher.server.ApkPackage");
        Object target=apk.getMethod("parse",String.class).invoke(null,args[0]);
        Object sourceApk=apk.getMethod("parse",String.class).invoke(null,args[1]);
        String targetName=(String)apk.getField("packageName").get(target),sourceName=(String)apk.getField("packageName").get(sourceApk);
        Class<?> pm=Class.forName("com.gesher.server.PackageManagerService"),atm=Class.forName("com.gesher.server.ActivityTaskManagerService");
        Class<?> am=Class.forName("com.gesher.server.ActivityManagerService"),notification=Class.forName("com.gesher.server.NotificationManagerService");
        Object packages=pm.getConstructor(Collection.class).newInstance(Arrays.asList(target,sourceApk));
        Object tasks=make(atm,new Class<?>[]{pm},packages);
        make(am,new Class<?>[]{atm,pm,notification},tasks,packages,make(notification,new Class<?>[0]));
        Class<?> processType=Class.forName(atm.getName()+"$ProcessRecord"),recordType=Class.forName(atm.getName()+"$ActivityRecord");
        ThreadStub thread=new ThreadStub();
        Object sourceProcess=make(processType,new Class<?>[]{atm,int.class,IApplicationThread.class},tasks,Binder.getCallingPid(),thread);
        set(sourceProcess,"packageName",sourceName);set(sourceProcess,"processName",sourceName);set(sourceProcess,"windowMode","window");
        set(sourceProcess,"uid",method(apk,"uid").invoke(sourceApk));
        ((List<Object>)get(tasks,"mProcesses")).add(sourceProcess);
        ActivityInfo sourceInfo=(ActivityInfo)apk.getMethod("launcherActivity",String.class).invoke(sourceApk,(Object)null);
        Object source=make(recordType,new Class<?>[]{atm,ActivityInfo.class,Intent.class,int.class,int.class,String.class,recordType,String.class,int.class},
                tasks,sourceInfo,new Intent(),1,Binder.getCallingPid(),sourceName,null,null,-1);
        set(source,"app",sourceProcess);Class<?> state=Class.forName(atm.getName()+"$State");set(source,"state",Enum.valueOf((Class)state,"PAUSED"));
        ((List<Object>)get(tasks,"mActivities")).add(source);
        IBinder sourceToken=(IBinder)get(source,"token");
        ActivityInfo targetInfo=(ActivityInfo)apk.getMethod("launcherActivity",String.class).invoke(target,(Object)null);
        Intent launch=new Intent().setComponent(new ComponentName(targetName,targetInfo.name)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        // These untrusted extras must never be read as process policy.
        launch.putExtra("windowMode","window").putExtra("GESHER_APP_WINDOW","window");
        Class<?> wm=Class.forName("com.gesher.server.WindowManagerService");
        Method setMode=wm.getMethod("setWindowMode",String.class,String.class),getMode=wm.getMethod("windowMode",String.class);
        Method resolve=method(wm,"resolveWindowMode",String.class,String.class);
        Class<?> starterType=Class.forName(atm.getName()+"$ProcessStarter");
        AtomicReference<String> mode=new AtomicReference<>();AtomicInteger starts=new AtomicInteger();
        Object starter=Proxy.newProxyInstance(starterType.getClassLoader(),new Class<?>[]{starterType},(p,m,a)->{
            require(m.getName().equals("start") && a.length==5,"Resolved mode was not passed to process starter");
            mode.set((String)a[4]);starts.incrementAndGet();
            Map<?,?> pending=(Map<?,?>)get(tasks,"mStarting");Object request=pending.get(a[2]);
            require(request!=null && a[4].equals(get(request,"windowMode")),"Pending process did not retain exact spawn mode");
            require((Boolean)get(request,"windowModeExplicit")==(getMode.invoke(null,targetName)!=null),
                    "Pending process lost explicit/default mode provenance");
            throw new IllegalStateException("Synthetic starter stops before native spawn");
        });
        method(atm,"setProcessStarter",starterType).invoke(tasks,starter);
        IActivityTaskManager api=(IActivityTaskManager)tasks;
        try{
            setMode.invoke(null,targetName,null);setMode.invoke(null,sourceName,"hidden");
            api.startActivity(thread,sourceName,null,launch,null,sourceToken,null,-1,0,null,null);
            require("window".equals(mode.get()),"Fresh target did not inherit actual source process mode");
            require(getMode.invoke(null,targetName)==null,"Inheritance persisted into explicit host policy");
            for(String explicit:Arrays.asList("headless","hidden","window")){
                setMode.invoke(null,targetName,explicit);
                api.startActivity(thread,sourceName,null,launch,null,sourceToken,null,-1,0,null,null);
                require(explicit.equals(mode.get()),"Explicit target policy was overridden: "+explicit);
            }
            setMode.invoke(null,targetName,null);
            for(String inherited:Arrays.asList("headless","hidden")){
                set(sourceProcess,"windowMode",inherited);
                api.startActivity(thread,sourceName,null,launch,null,sourceToken,null,-1,0,null,null);
                require(inherited.equals(mode.get()),"Hidden/headless source was promoted");
            }
            set(sourceProcess,"windowMode","window");
            api.startActivity(new ThreadStub(),sourceName,null,launch,null,sourceToken,null,-1,0,null,null);
            require("headless".equals(mode.get()),"Forged caller thread inherited visibility");
            api.startActivity(thread,targetName,null,launch,null,sourceToken,null,-1,0,null,null);
            require("headless".equals(mode.get()),"Forged source package inherited visibility");
            api.startActivity(thread,sourceName,null,launch,null,new Binder(),null,-1,0,null,null);
            require("headless".equals(mode.get()),"Forged activity token inherited visibility");
            api.startActivity(thread,sourceName,null,launch,null,null,null,-1,0,null,null);
            require("headless".equals(mode.get()),"Background context without an activity inherited visibility");
            // Application/Context starts omit resultTo. A currently front, live
            // activity still authenticates their process; a paused app does not.
            for(String foreground:Arrays.asList("LAUNCHING","RESUMING","RESUMED")){
                set(source,"state",Enum.valueOf((Class)state,foreground));
                api.startActivity(thread,sourceName,null,launch,null,null,null,-1,0,null,null);
                require("window".equals(mode.get()),"Tokenless foreground/launching caller lost mode: "+foreground);
            }
            api.startActivity(new ThreadStub(),sourceName,null,launch,null,null,null,-1,0,null,null);
            require("headless".equals(mode.get()),"Tokenless forged caller thread inherited visibility");
            api.startActivity(thread,targetName,null,launch,null,null,null,-1,0,null,null);
            require("headless".equals(mode.get()),"Tokenless forged package inherited visibility");
            api.startActivity(thread,sourceName,null,launch,null,new Binder(),null,-1,0,null,null);
            require("headless".equals(mode.get()),"Forged non-null token fell back to foreground authority");
            ((Set<Integer>)get(tasks,"mHiddenTasks")).add(1);
            api.startActivity(thread,sourceName,null,launch,null,null,null,-1,0,null,null);
            require("headless".equals(mode.get()),"Hidden task authorized a tokenless visible launch");
            ((Set<Integer>)get(tasks,"mHiddenTasks")).remove(1);
            Object covering=make(recordType,new Class<?>[]{atm,ActivityInfo.class,Intent.class,int.class,int.class,String.class,recordType,String.class,int.class},
                    tasks,sourceInfo,new Intent(),1,Binder.getCallingPid(),sourceName,null,null,-1);
            ((List<Object>)get(tasks,"mActivities")).add(covering);
            api.startActivity(thread,sourceName,null,launch,null,null,null,-1,0,null,null);
            require("headless".equals(mode.get()),"Covered source authorized a tokenless visible launch");
            ((List<Object>)get(tasks,"mActivities")).remove(covering);
            setMode.invoke(null,targetName,"headless");
            api.startActivity(thread,sourceName,null,launch,null,null,null,-1,0,null,null);
            require("headless".equals(mode.get()),"Tokenless foreground caller overrode explicit headless target");
            setMode.invoke(null,targetName,null);
            for(String background:Arrays.asList("PAUSING","PAUSED","STOPPED","INITIALIZING","DESTROYING","DESTROYED")){
                set(source,"state",Enum.valueOf((Class)state,background));
                api.startActivity(thread,sourceName,null,launch,null,null,null,-1,0,null,null);
                require("headless".equals(mode.get()),"Background lifecycle authorized a tokenless visible launch: "+background);
            }
            set(source,"state",Enum.valueOf((Class)state,"PAUSED"));
            int pid=(Integer)get(sourceProcess,"pid");set(sourceProcess,"pid",pid+100000);
            api.startActivity(thread,sourceName,null,launch,null,sourceToken,null,-1,0,null,null);
            require("headless".equals(mode.get()),"Forged caller PID inherited visibility");set(sourceProcess,"pid",pid);
            set(source,"finishing",true);
            api.startActivity(thread,sourceName,null,launch,null,sourceToken,null,-1,0,null,null);
            require("headless".equals(mode.get()),"Finishing activity inherited visibility");set(source,"finishing",false);
            set(sourceProcess,"exiting",true);
            api.startActivity(thread,sourceName,null,launch,null,sourceToken,null,-1,0,null,null);
            require("headless".equals(mode.get()),"Exiting process inherited visibility");set(sourceProcess,"exiting",false);
            set(sourceProcess,"windowMode",null);
            api.startActivity(thread,sourceName,null,launch,null,sourceToken,null,-1,0,null,null);
            require("headless".equals(mode.get()),"Unknown legacy source mode was inferred from mutable host settings");
            set(sourceProcess,"windowMode","window");
            method(atm,"startProcess",String.class,String.class,String.class).invoke(tasks,targetName,targetName,"background fixture");
            require("headless".equals(mode.get()) && getMode.invoke(null,targetName)==null,
                    "Background service/provider start reused transient activity mode");
            require("hidden".equals(resolve.invoke(null,targetName,"hidden")),"Hidden inherited fallback changed");
            require("headless".equals(resolve.invoke(null,targetName,"invalid")),"Invalid inherited mode bypassed device default");
            // An already-running target is neither restarted nor promoted.
            Object existing=make(processType,new Class<?>[]{atm,int.class,IApplicationThread.class},tasks,pid+200000,new ThreadStub());
            set(existing,"packageName",targetName);set(existing,"processName",targetName);set(existing,"windowMode","headless");
            ((List<Object>)get(tasks,"mProcesses")).add(existing);int before=starts.get();
            api.startActivity(thread,sourceName,null,launch,null,sourceToken,null,-1,0,null,null);
            require(starts.get()==before && "headless".equals(get(existing,"windowMode")),"Existing target was restarted or promoted");
        }finally{setMode.invoke(null,targetName,null);setMode.invoke(null,sourceName,null);}
        System.out.println("WindowModeInheritanceTest all passed: "+checks+" authenticated route and fresh-process mode checks");
    }
}
