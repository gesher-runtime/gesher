/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.IApplicationThread;
import android.app.servertransaction.GesherActivityWindowModeItem;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.res.AssetManager;
import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.view.IWindow;
import android.view.WindowManager;
import java.lang.reflect.*;
import java.nio.file.Files;
import java.util.*;

/** Synthetic private capability and selective-window policy; no native windows are opened. */
public final class ActivityWindowPromotionTest {
    private static int checks;
    private static void require(boolean v,String why){if(!v)throw new AssertionError(why);checks++;}
    private static Field field(Class<?> c,String n)throws Exception{Field f=c.getDeclaredField(n);f.setAccessible(true);return f;}
    private static Object get(Object o,String n)throws Exception{return field(o.getClass(),n).get(o);}
    private static void set(Object o,String n,Object v)throws Exception{field(o.getClass(),n).set(o,v);}
    private static Method method(Class<?> c,String n,Class<?>...a)throws Exception{Method m=c.getDeclaredMethod(n,a);m.setAccessible(true);return m;}
    private static Object make(Class<?> c,Class<?>[] a,Object...v)throws Exception{Constructor<?> k=c.getDeclaredConstructor(a);k.setAccessible(true);return k.newInstance(v);}
    private interface Op {void run()throws Exception;}
    private static void denied(Op op)throws Exception{try{op.run();}catch(SecurityException e){checks++;return;}
        catch(InvocationTargetException e){if(e.getCause() instanceof SecurityException){checks++;return;}throw e;}
        throw new AssertionError("Unauthorized lifecycle capability accepted");}
    private static final class ThreadStub extends IApplicationThread.Default{
        final IBinder token=new Binder();@Override public IBinder asBinder(){return token;}
    }
    @SuppressWarnings({"unchecked","rawtypes"})
    public static void main(String[] args)throws Exception{
        if(args.length==1 && args[0].equals("client-denied")){
            System.loadLibrary("android_runtime");
            require(Binder.getCallingUid()!=android.os.Process.SYSTEM_UID,"Client denial needs ordinary UID");
            denied(()->new GesherActivityWindowModeItem(new Binder(),new Binder()).preExecute(null));
            System.out.println("ActivityWindowPromotionTest client role denied");return;
        }
        require(args.length==1,"Pass a metadata-only fixture APK");
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false,System.getenv("ANDROID_ROOT")+"/framework/framework-res.apk");
        android.system.Os.setenv("ANDROID_DATA",Files.createTempDirectory("activity-window-").toString(),true);
        android.system.Os.setenv("GESHER_MICROG_SPOOF","1",true);
        Class<?> apk=Class.forName("com.gesher.server.ApkPackage"),pm=Class.forName("com.gesher.server.PackageManagerService");
        Object code=apk.getMethod("parse",String.class).invoke(null,args[0]);String pkg=(String)apk.getField("packageName").get(code);
        Object packages=pm.getConstructor(Collection.class).newInstance(Arrays.asList(code));
        Class<?> atm=Class.forName("com.gesher.server.ActivityTaskManagerService");
        Object tasks=make(atm,new Class<?>[]{pm},packages);
        Class<?> processType=Class.forName(atm.getName()+"$ProcessRecord"),recordType=Class.forName(atm.getName()+"$ActivityRecord");
        Object process=make(processType,new Class<?>[]{atm,int.class,IApplicationThread.class},tasks,Binder.getCallingPid(),new ThreadStub());
        set(process,"packageName",pkg);set(process,"processName",pkg);set(process,"windowMode","headless");
        ((List<Object>)get(tasks,"mProcesses")).add(process);
        ActivityInfo info=(ActivityInfo)apk.getMethod("launcherActivity",String.class).invoke(code,(Object)null);
        Object record=make(recordType,new Class<?>[]{atm,ActivityInfo.class,Intent.class,int.class,int.class,String.class,recordType,String.class,int.class},
                tasks,info,new Intent(),1,Binder.getCallingPid(),pkg,null,null,-1);
        set(record,"app",process);set(record,"inheritedWindowMode","window");
        ((List<Object>)get(tasks,"mActivities")).add(record);
        IBinder token=(IBinder)get(record,"token");
        Class<?> wm=Class.forName("com.gesher.server.WindowManagerService");
        Method setMode=wm.getMethod("setWindowMode",String.class,String.class);
        Method create=method(atm,"createActivityWindowGrant",recordType);
        try{
            setMode.invoke(null,pkg,null);
            IBinder grant=(IBinder)create.invoke(tasks,record);
            require(grant!=null && "window".equals(GesherActivityWindowModeItem.resolve(grant,token)),"Implicit headless process could not resolve approved activity mode");
            require("headless".equals(get(process,"windowMode")) && wm.getMethod("windowMode",String.class).invoke(null,pkg)==null,
                    "Activity promotion mutated startup mode or host policy");
            require(GesherActivityWindowModeItem.resolve(grant,new Binder())==null,"Another activity token used the capability");
            require("window".equals(GesherActivityWindowModeItem.resolve(grant,token)),"Bad token permanently consumed valid capability");
            setMode.invoke(null,pkg,"headless");
            require(GesherActivityWindowModeItem.resolve(grant,token)==null,"Late explicit headless policy was overridden");
            setMode.invoke(null,pkg,"hidden");
            require("hidden".equals(GesherActivityWindowModeItem.resolve(grant,token)),"Explicit hidden policy became visible");
            setMode.invoke(null,pkg,null);set(process,"windowModeExplicit",true);
            require(GesherActivityWindowModeItem.resolve(grant,token)==null,"Explicit startup headless provenance was lost");
            require(create.invoke(tasks,record)==null,"Explicit headless process received a new inherited grant");
            set(process,"windowModeExplicit",false);
            set(record,"finishing",true);require(GesherActivityWindowModeItem.resolve(grant,token)==null,"Finishing activity retained grant");set(record,"finishing",false);
            set(process,"exiting",true);require(GesherActivityWindowModeItem.resolve(grant,token)==null,"Exiting target retained grant");set(process,"exiting",false);
            Class<?> state=Class.forName(atm.getName()+"$State");set(record,"state",Enum.valueOf((Class)state,"DESTROYED"));
            require(GesherActivityWindowModeItem.resolve(grant,token)==null,"Destroyed activity retained grant");set(record,"state",Enum.valueOf((Class)state,"INITIALIZING"));
            int pid=(Integer)get(process,"pid"),uid=(Integer)get(process,"uid");
            set(process,"pid",pid+1);denied(()->GesherActivityWindowModeItem.resolve(grant,token));set(process,"pid",pid);
            set(process,"uid",uid+1);denied(()->GesherActivityWindowModeItem.resolve(grant,token));set(process,"uid",uid);
            set(record,"inheritedWindowMode",null);require(create.invoke(tasks,record)==null,"Background activity without source authority was promoted");
            set(record,"inheritedWindowMode","hidden");IBinder hidden=(IBinder)create.invoke(tasks,record);
            require(hidden!=null && "hidden".equals(GesherActivityWindowModeItem.resolve(hidden,token)),"Hidden source lost offscreen graphics mode");
            require(GesherActivityWindowModeItem.resolve(grant,token)==null,"Replaced capability remained valid");
            set(process,"windowMode","window");require(create.invoke(tasks,record)==null,"Already graphical process entered headless promotion");set(process,"windowMode","headless");
            Object replacement=apk.getMethod("parse",String.class).invoke(null,args[0]);method(pm,"add",apk).invoke(packages,replacement);
            require(GesherActivityWindowModeItem.resolve(hidden,token)==null,"Package replacement inherited a pending capability");
            // A parcel round trip cannot carry the sender-authenticated bit.
            GesherActivityWindowModeItem item=new GesherActivityWindowModeItem(token,hidden);
            denied(()->item.execute(null,null));item.preExecute(null);
            Parcel parcel=Parcel.obtain();item.writeToParcel(parcel,0);parcel.setDataPosition(0);
            GesherActivityWindowModeItem copy=GesherActivityWindowModeItem.CREATOR.createFromParcel(parcel);parcel.recycle();
            require(copy.getActivityToken().equals(token),"Typed lifecycle token lost in parcel");denied(()->copy.execute(null,null));
            // Selective sessions scope visibility to activity tokens and descendants.
            Class<?> sessionType=Class.forName("com.gesher.app.window.WindowSession");
            Object session=make(sessionType,new Class<?>[]{boolean.class},false);
            Class<?> windowType=Class.forName(sessionType.getName()+"$AppWindow");
            Object root=make(windowType,new Class<?>[]{IWindow.class},(Object)null);
            Object popup=make(windowType,new Class<?>[]{IWindow.class},(Object)null);
            Object unrelated=make(windowType,new Class<?>[]{IWindow.class},(Object)null);
            ((WindowManager.LayoutParams)get(root,"attrs")).token=token;
            ((WindowManager.LayoutParams)get(popup,"attrs")).token=new Binder();set(popup,"parent",root);
            ((WindowManager.LayoutParams)get(unrelated,"attrs")).token=new Binder();
            Method shown=method(sessionType,"shownOnDesktop",windowType);
            require(!(Boolean)shown.invoke(session,root),"Selective session default became visible");
            sessionType.getMethod("setActivityVisibility",IBinder.class,boolean.class).invoke(session,token,true);
            require((Boolean)shown.invoke(session,root) && (Boolean)shown.invoke(session,popup),"Approved activity and popup were not scoped together");
            require(!(Boolean)shown.invoke(session,unrelated),"Unapproved root inherited another activity's visibility");
            sessionType.getMethod("setActivityVisibility",IBinder.class,boolean.class).invoke(session,token,false);
            require(!(Boolean)shown.invoke(session,root) && !(Boolean)shown.invoke(session,popup),"Hidden activity was promoted");
            sessionType.getMethod("setActivityVisibility",IBinder.class,boolean.class).invoke(session,token,true);
            sessionType.getMethod("revokeActivityVisibility",IBinder.class).invoke(session,token);
            require(!(Boolean)shown.invoke(session,root) && !(Boolean)shown.invoke(session,popup),"Destroyed activity approval leaked to descendants");
        }finally{setMode.invoke(null,pkg,null);}
        System.out.println("ActivityWindowPromotionTest all passed: "+checks+" capability, lifecycle and selective-root checks");
    }
}
