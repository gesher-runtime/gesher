"""Minimal Android I/O and identity boundaries, never replacement observer behavior."""

SOURCES = {
    "android/app/ActivityManager.java": '''package android.app;
        public final class ActivityManager { public static final int PROCESS_STATE_TOP=2; }''',
    "android/Manifest.java": '''package android; public final class Manifest {
        public static final class permission {
            public static final String PACKAGE_USAGE_STATS = "android.permission.PACKAGE_USAGE_STATS";
            public static final String OBSERVE_APP_USAGE = "android.permission.OBSERVE_APP_USAGE";
        }
    }''',
    "android/app/AppOpsManager.java": '''package android.app; public final class AppOpsManager {
        public static final int OP_GET_USAGE_STATS=43, MODE_DEFAULT=3, MODE_ERRORED=2, MODE_ALLOWED=0;
    }''',
    "android/app/PendingIntent.java": '''package android.app;
        import android.content.IIntentSender;
        public final class PendingIntent {
            private final IIntentSender target;
            public PendingIntent(IIntentSender target) { this.target=target; }
            public IIntentSender getTarget() { return target; }
        }''',
    "android/content/IIntentSender.java": '''package android.content;
        public interface IIntentSender {
            void send(int code, Intent intent, String type, Object whitelist, Object receiver,
                    String permission, Object options) throws android.os.RemoteException;
        }''',
    "android/content/Intent.java": '''package android.content;
        import java.util.*;
        public final class Intent {
            public final Map<String,Object> extras=new HashMap<>();
            public Intent putExtra(String name, int value) { extras.put(name,value);return this; }
            public Intent putExtra(String name, long value) { extras.put(name,value);return this; }
            public int getIntExtra(String name, int fallback) { return (int) extras.getOrDefault(name,fallback); }
            public long getLongExtra(String name, long fallback) { return (long) extras.getOrDefault(name,fallback); }
        }''',
    "android/content/pm/PackageManager.java": '''package android.content.pm;
        public class PackageManager { public static final int PERMISSION_GRANTED=0,PERMISSION_DENIED=-1; }''',
    "android/content/pm/ParceledListSlice.java": '''package android.content.pm;
        import java.util.*;
        public class ParceledListSlice<T> {
            public ParceledListSlice(List<T> values) { }
            public static <T> ParceledListSlice<T> emptyList() { return new ParceledListSlice<>(List.of()); }
        }''',
    "android/app/usage/AppStandbyInfo.java": '''package android.app.usage;
        public class AppStandbyInfo { public AppStandbyInfo(String pkg,int bucket) { } }''',
    "android/app/usage/BroadcastResponseStatsList.java": '''package android.app.usage;
        public class BroadcastResponseStatsList { public BroadcastResponseStatsList(java.util.List<?> values) { } }''',
    "android/app/usage/UsageStats.java": '''package android.app.usage;
        public class UsageStats {
            public String mPackageName;
            public long mBeginTimeStamp,mEndTimeStamp,mTotalTimeInForeground,mTotalTimeVisible,mLastTimeUsed,mLastTimeVisible;
            public int mLaunchCount,mAppLaunchCount,mLastEvent;
        }''',
    "android/app/usage/UsageStatsManager.java": '''package android.app.usage;
        public class UsageStatsManager {
            public static final int INTERVAL_DAILY=0,INTERVAL_WEEKLY=1,INTERVAL_MONTHLY=2,INTERVAL_YEARLY=3,INTERVAL_BEST=4;
            public static final int STANDBY_BUCKET_ACTIVE=10,USAGE_SOURCE_TASK_ROOT_ACTIVITY=1;
            public static final String EXTRA_OBSERVER_ID="android.app.usage.extra.OBSERVER_ID";
            public static final String EXTRA_TIME_LIMIT="android.app.usage.extra.TIME_LIMIT";
            public static final String EXTRA_TIME_USED="android.app.usage.extra.TIME_USED";
        }''',
    "android/app/usage/UsageEvents.java": '''package android.app.usage;
        public class UsageEvents {
            public UsageEvents(java.util.List<Event> events, String[] strings) { }
            public static class Event {
                public static final int NONE=0,ACTIVITY_RESUMED=1,ACTIVITY_PAUSED=2,ACTIVITY_STOPPED=23;
                public String mPackage,mClass;
                public int mInstanceId,mEventType;
                public long mTimeStamp;
            }
        }''',
    "android/app/usage/UsageEventsQuery.java": '''package android.app.usage;
        public class UsageEventsQuery {
            public int getUserId(){return 0;} public long getBeginTimeMillis(){return 0;}
            public long getEndTimeMillis(){return Long.MAX_VALUE;}
            public java.util.Set<String> getPackageNames(){return java.util.Set.of();}
            public int[] getEventTypes(){return new int[0];}
        }''',
    "android/os/Binder.java": '''package android.os;
        public class Binder implements IBinder {
            public static boolean cleared;
            public static long clearCallingIdentity(){long old=cleared?1:0;cleared=true;return old;}
            public static void restoreCallingIdentity(long value){cleared=value==1;}
        }''',
    "android/os/SystemClock.java": '''package android.os;
        public class SystemClock { public static long elapsedRealtime(){return System.nanoTime()/1_000_000;} }''',
    "android/os/UserHandle.java": '''package android.os;
        public class UserHandle { public static final int USER_NULL=-10000; }''',
    "com/gesher/server/ObserverBoundaries.java": '''package com.gesher.server;
        import java.util.*;
        import java.util.function.Consumer;
        import android.app.AppOpsManager;
        final class ApkPackage {
            final String packageName; final int uid;
            ApkPackage(String name,int uid){packageName=name;this.uid=uid;}
            int uid(){return uid;}
        }
        final class PackageManagerService {
            final Map<String,ApkPackage> installed=new HashMap<>();
            final Set<String> pending=new HashSet<>();
            final List<Consumer<String>> removals=new ArrayList<>();
            final Permissions permissions=new Permissions();
            ApkPackage get(String name){return installed.get(name);}
            boolean isRemovalPending(String name){return pending.contains(name);}
            Object getPackageInfo(String name,int flags,int user){return get(name);}
            Collection<ApkPackage> packages(){return installed.values();}
            AutoCloseable onPackageRemoval(Consumer<String> listener){removals.add(listener);return ()->removals.remove(listener);}
            Permissions permissions(){return permissions;}
            Operations appOps(){return new Operations();}
        }
        final class Permissions {
            Object atm;
            boolean granted; String lastPermission; int lastUid,lastPid;
            int checkProcessPermission(String permission,int pid,int uid){
                // The real policy authenticates the PID through synchronized ATM.
                synchronized(atm){lastPermission=permission;lastUid=uid;lastPid=pid;return granted?0:-1;}
            }
        }
        final class Operations {
            Operations noteOperation(int op,int uid,String pkg,Object tag,boolean collect,Object message,boolean log){return this;}
            int getOpMode(){return AppOpsManager.MODE_ALLOWED;}
        }
        final class CallerIdentity {
            final PackageManagerService pm; final ApkPackage owner; final int pid=73;
            final Object atm=new Object();
            volatile Runnable beforeIdentity;
            java.util.function.Consumer<String> afterCurrentPackage;
            CallerIdentity(PackageManagerService pm,ApkPackage owner){this.pm=pm;this.owner=owner;pm.permissions.atm=atm;}
            int callerUid(){return owner.uid();} int callerPid(){return pid;}
            boolean isSystem(){synchronized(atm){return false;}}
            String requirePackage(String caller){
                Runnable hook=beforeIdentity;if(hook!=null)hook.run();
                // Identity crosses the ATM monitor in the real CallerIdentity.
                synchronized(atm){
                    ApkPackage claimed=pm.get(caller);
                    if(pm.get(owner.packageName)!=owner||pm.isRemovalPending(owner.packageName)
                            ||claimed==null||claimed.uid()!=owner.uid()||pm.isRemovalPending(caller))
                        throw new SecurityException("Unrecognized owner");return owner.packageName;
                }
            }
            ApkPackage requireCurrentPackage(String caller){
                requirePackage(caller);ApkPackage captured=pm.get(caller);
                var hook=afterCurrentPackage;if(hook!=null)hook.accept(caller);return captured;
            }
        }
        final class SystemServer { static void log(String message){ } }
    ''',
}

for name in ("ConfigurationStats", "EventStats"):
    SOURCES["android/app/usage/" + name + ".java"] = "package android.app.usage; public class " + name + " {}"
for name in ("PersistableBundle", "RemoteException"):
    SOURCES["android/os/" + name + ".java"] = "package android.os; public class " + name + (
        " extends Exception {}" if name == "RemoteException" else " {}")
SOURCES["android/os/IBinder.java"] = "package android.os; public interface IBinder {}"
