/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.content.ComponentName;
import android.content.ContextWrapper;
import android.content.Intent;
import android.content.pm.IShortcutService;
import android.content.pm.ParceledListSlice;
import android.content.pm.ShortcutInfo;
import android.content.res.AssetManager;
import android.os.Binder;
import com.gesher.app.shell.AppShell;
import com.gesher.app.shell.Shell;
import com.gesher.server.ApkPackage;
import com.gesher.server.CallerIdentity;
import com.gesher.server.PackageManagerService;
import com.gesher.server.Zygote;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;

/** Real Binder identity, durable metadata, bounded host arguments and owned shell files. */
public final class DynamicShortcutTest {
    private static int checks;
    private static void require(boolean value,String message) { if(!value)throw new AssertionError(message);checks++; }
    private interface Action { void run() throws Exception; }
    private static void denied(Action action) throws Exception {
        try {action.run();throw new AssertionError("forged shortcut publisher accepted");}
        catch(SecurityException expected){checks++;}
    }
    private static Object field(Object object,String name) throws Exception {
        Field field=object.getClass().getDeclaredField(name);field.setAccessible(true);return field.get(object);
    }
    private static Object make(Class<?> type,Class<?>[] types,Object...args) throws Exception {
        Constructor<?> constructor=type.getDeclaredConstructor(types);constructor.setAccessible(true);return constructor.newInstance(args);
    }
    private static IShortcutService service(PackageManagerService pm,CallerIdentity callers) throws Exception {
        return (IShortcutService)make(Class.forName("com.gesher.server.DeviceServices$ShortcutService"),
                new Class<?>[]{PackageManagerService.class,CallerIdentity.class},pm,callers);
    }
    private static ShortcutInfo info(String pkg,String id,String label) {
        return new ShortcutInfo.Builder(new ContextWrapper(null){
            @Override public String getPackageName(){return pkg;}
            @Override public int getUserId(){return 0;}
        },id).setShortLabel(label).setIntent(new Intent(Intent.ACTION_VIEW)
                .setComponent(new ComponentName(pkg,pkg+".SyntheticActivity"))
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION|Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION))
                .setRank(3).build();
    }
    @SuppressWarnings("unchecked")
    private static List<ShortcutInfo> list(IShortcutService api,String pkg) throws Exception {
        return api.getShortcuts(pkg,15,0).getList();
    }
    public static void main(String[] args) throws Exception {
        if(args.length==2 && args[0].equals("--publish")) {
            System.loadLibrary("android_runtime");
            android.content.pm.IPackageManager pm=android.content.pm.IPackageManager.Stub.asInterface(android.os.ServiceManager.getService("package"));
            android.content.pm.PackageInfo installed=pm.getPackageInfo(args[1],android.content.pm.PackageManager.GET_ACTIVITIES,0);
            require(installed!=null && installed.activities!=null && installed.activities.length>0,"CLI fixture has no activity");
            ShortcutInfo shortcut=info(args[1],"CLI \" synthetic shortcut","synthetic CLI shortcut");
            shortcut.setIntents(new Intent[]{new Intent(Intent.ACTION_MAIN).setComponent(new ComponentName(args[1],installed.activities[0].name))});
            IShortcutService api=IShortcutService.Stub.asInterface(android.os.ServiceManager.getService("shortcut"));
            require(api.setDynamicShortcuts(args[1],new ParceledListSlice<>(Collections.singletonList(shortcut)),0),"CLI shortcut publication failed");
            System.out.println("SHORTCUT_KEY="+AppShell.dynamicKey(args[1],shortcut.getId()));
            return;
        }
        require(args.length==2,"Pass ordinary source and target APK fixtures");
        System.loadLibrary("android_runtime");
        ZygotePathTest.contracts();
        AssetManager.createSystemAssetsInZygoteLocked(false,System.getenv("ANDROID_ROOT")+"/framework/framework-res.apk");
        android.system.Os.setenv("GESHER_MICROG_SPOOF","1",true);
        android.system.Os.setenv("GESHER_SHORTCUTS","0",true);
        Object harness=make(Class.forName("gesher.services.DelegatedIntentRetirementTest$Harness"),
                new Class<?>[]{String.class,String.class,boolean.class,boolean.class},args[0],args[1],false,true);
        PackageManagerService pm=(PackageManagerService)field(harness,"pm");
        ApkPackage owner=(ApkPackage)field(harness,"target"),other=(ApkPackage)field(harness,"creator");
        Object atm=field(harness,"atm");
        CallerIdentity callers=(CallerIdentity)make(CallerIdentity.class,new Class<?>[]{PackageManagerService.class,atm.getClass()},pm,atm);
        IShortcutService api=service(pm,callers);
        String pkg=owner.packageName;
        String id="synthetic \" --activity other.Activity \\ path";
        ShortcutInfo first=info(pkg,id,"קיצור בדיקה");
        long original=Binder.clearCallingIdentity();
        try {
            Binder.restoreCallingIdentity(((long)owner.uid()<<32)|910001);
            denied(()->api.setDynamicShortcuts(other.packageName,new ParceledListSlice<>(Collections.singletonList(info(other.packageName,"foreign","foreign"))),0));
            require(api.setDynamicShortcuts(pkg,new ParceledListSlice<>(Collections.singletonList(first)),0),"valid snapshot failed");
            require(list(api,pkg).size()==1 && list(api,pkg).get(0).isDynamic(),"dynamic state missing");
            require((list(api,pkg).get(0).getIntent().getFlags()&Intent.IMMUTABLE_FLAGS)==0,"host URI grants inherited");
            first.setRank(99);
            require(list(api,pkg).get(0).getRank()==3,"caller mutated published state");
            require(list(service(pm,callers),pkg).size()==1,"new service lost durable metadata");
            String key=AppShell.dynamicKey(pkg,id),command=AppShell.dynamicArguments(pkg,id);
            require(key.matches("[0-9a-f]{64}") && command.equals("launch "+pkg+" --shortcut "+key)
                    && !command.contains(id),"arbitrary ID entered host argument syntax");
            Intent launch=AppShell.dynamicShortcutIntent(pkg,key,owner.codeIdentity());
            require(pkg.equals(launch.getComponent().getPackageName()) && pkg.equals(launch.getPackage()),"shortcut escaped publisher");
            denied(()->AppShell.dynamicShortcutIntent(pkg,key,"different-installed-code"));
            denied(()->AppShell.dynamicIntent(pkg,new Intent().setComponent(new ComponentName(other.packageName,"Other"))));
            Intent selector=new Intent();selector.setSelector(new Intent().setPackage(other.packageName));
            denied(()->AppShell.dynamicIntent(pkg,selector));
            denied(()->AppShell.dynamicIntent(pkg,Intent.createChooser(new Intent().setPackage(other.packageName),"synthetic")));
            require(api.addDynamicShortcuts(pkg,new ParceledListSlice<>(Collections.singletonList(info(pkg,"second","second"))),0),"add failed");
            require(api.updateShortcuts(pkg,new ParceledListSlice<>(Collections.singletonList(info(pkg,"unknown","unknown"))),0)
                    && list(api,pkg).size()==2,"update created an unknown ID");
            api.pushDynamicShortcut(pkg,info(pkg,"third","third"),0);
            require(list(api,pkg).size()==3,"push failed");
            api.disableShortcuts(pkg,Collections.singletonList(id),null,0,0);
            try {AppShell.dynamicShortcutIntent(pkg,key,owner.codeIdentity());throw new AssertionError("disabled shortcut launched");}
            catch(IllegalArgumentException expected){checks++;}
            api.enableShortcuts(pkg,Collections.singletonList(id),0);
            require(AppShell.dynamicShortcutIntent(pkg,key,owner.codeIdentity())!=null,"enabled shortcut missing");
            api.removeDynamicShortcuts(pkg,Collections.singletonList("second"),0);
            require(list(api,pkg).size()==2,"remove failed");
            List<ShortcutInfo> tooMany=new ArrayList<>();for(int i=0;i<16;i++)tooMany.add(info(pkg,"n"+i,"n"+i));
            try {api.setDynamicShortcuts(pkg,new ParceledListSlice<>(tooMany),0);throw new AssertionError("unbounded snapshot accepted");}
            catch(IllegalArgumentException expected){checks++;}
            require(list(api,pkg).size()==2,"rejected batch changed state");
            Binder.restoreCallingIdentity(((long)owner.uid()<<32)|999991);
            denied(()->api.getShortcuts(pkg,15,0));
            Binder.restoreCallingIdentity(((long)owner.uid()<<32)|910001);
            String folder=Shell.programsFolder()+"\\"+AppShell.FOLDER;
            String name="safe ["+key.substring(0,32)+"].lnk";
            require(AppShell.ownedDynamicLink(pkg,id,folder+"\\"+name),"owned file rejected");
            for(String language:new String[]{"en","he"}) require(AppShell.ownedDynamicLink(pkg,id,
                    Shell.programsFolder()+"\\"+com.gesher.app.locale.UiMessages.text("apps_folder",language)+"\\"+name),
                    "language change lost ownership of an existing shortcut");
            require(!AppShell.ownedDynamicLink(pkg,id,folder+"\\..\\"+name)
                    && !AppShell.ownedDynamicLink(pkg,id,"C:\\unrelated\\"+name)
                    && !AppShell.ownedDynamicLink(other.packageName,id,folder+"\\"+name),"cleanup escaped publisher ownership");
            Path temp=Files.createTempDirectory("dynamic-shell-");
            String link=Zygote.windowsPath(temp.resolve("קיצור בדיקה.lnk").toString());
            int hr=Shell.createShortcut(link,System.getenv("SystemRoot")+"\\System32\\cmd.exe",command,
                    Zygote.windowsPath(temp.toString()),"","synthetic shortcut QA",AppShell.appUserModelId(pkg));
            boolean linkExists=Shell.exists(link);
            require(hr==0 && linkExists,"actual Unicode ShellLink creation failed: HRESULT=0x"
                    +Integer.toHexString(hr)+", exists="+linkExists+", link="+link+", Java directory="+temp);
            require(Shell.removeFile(link),"owned test ShellLink cleanup failed");
            api.removeAllDynamicShortcuts(pkg,0);
            require(list(api,pkg).isEmpty(),"remove-all failed");
            try {AppShell.dynamicShortcutIntent(pkg,key,owner.codeIdentity());throw new AssertionError("stale shortcut fell back to another action");}
            catch(SecurityException|IllegalArgumentException expected){checks++;}
            Method hostStart=atm.getClass().getDeclaredMethod("startShortcutForHost",String.class,String.class);
            hostStart.setAccessible(true);
            try {hostStart.invoke(atm,pkg,key);throw new AssertionError("app invoked host shortcut control");}
            catch(java.lang.reflect.InvocationTargetException expected){require(expected.getCause() instanceof SecurityException,"host shortcut role failure changed");}
            long appIdentity=Binder.clearCallingIdentity();
            try {
                ShortcutInfo race=info(other.packageName,"race","race");
                race.setIntents(new Intent[]{new Intent(Intent.ACTION_MAIN).setComponent(new ComponentName(other.packageName,other.packageName+".SourceActivity"))});
                require(api.setDynamicShortcuts(other.packageName,new ParceledListSlice<>(Collections.singletonList(race)),0),"publisher-race setup failed");
                Field before=atm.getClass().getDeclaredField("mBeforeShortcutResolution");before.setAccessible(true);
                Method retire=harness.getClass().getDeclaredMethod("retire");retire.setAccessible(true);
                before.set(atm,(Runnable)()->{try{retire.invoke(harness);}catch(Exception failure){throw new AssertionError(failure);}});
                com.gesher.server.GesherService.LaunchResult outcome=(com.gesher.server.GesherService.LaunchResult)hostStart.invoke(atm,
                        other.packageName,AppShell.dynamicKey(other.packageName,"race"));
                require(!android.app.ActivityManager.isStartResultSuccessful(outcome.result.result),"shortcut inherited replaced publisher code before resolution");
                before.set(atm,null);
            } finally {Binder.restoreCallingIdentity(appIdentity);}
            require(api.addDynamicShortcuts(pkg,new ParceledListSlice<>(Collections.singletonList(info(pkg,"retirement","retirement"))),0),"retirement setup failed");
            Method revoke=PackageManagerService.class.getDeclaredMethod("revokePackageCapabilities",String.class);
            revoke.setAccessible(true);revoke.invoke(pm,pkg);
            require(list(api,pkg).isEmpty(),"package retirement retained dynamic shortcuts");
            try {AppShell.dynamicShortcutIntent(pkg,AppShell.dynamicKey(pkg,"retirement"),owner.codeIdentity());throw new AssertionError("retired shortcut launched");}
            catch(SecurityException|IllegalArgumentException expected){checks++;}
        } finally {Binder.restoreCallingIdentity(original);}
        System.out.println("DynamicShortcutTest all passed: "+checks+" checks");
    }
}
