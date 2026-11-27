#!/usr/bin/env python3
"""Exercise production enrollment queue with explicit Android/PM/service/host-dialog boundaries.

This checks queueing, decisions and resolution cuts; it does not establish Android
Binder lifecycle, resource parsing, persistent admin state or Windows locking.
"""
import argparse
import subprocess
import tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
STUBS = {
'android/app/Activity.java': 'package android.app; public class Activity { public static final int RESULT_CANCELED=0, RESULT_OK=-1; }',
'android/content/ComponentName.java': 'package android.content; public record ComponentName(String p,String c) { public String getPackageName(){return p;} public String getClassName(){return c;} public String flattenToShortString(){return p+"/"+c;} }',
'android/content/Intent.java': """package android.content; public class Intent {
 public static final String CATEGORY_DEFAULT="android.intent.category.DEFAULT";
 public static final int FLAG_ACTIVITY_NEW_TASK=0x10000000;
 public ComponentName component; public Intent selector; public String pkg,action,type,data; public java.util.Set<String> cats; public Object extra; public CharSequence explanation; public int flags;
 public ComponentName getComponent(){return component;} public Intent getSelector(){return selector;}
 public String getPackage(){return pkg;} public String getAction(){return action;} public String getType(){return type;}
 public String getData(){return data;} public java.util.Set<String> getCategories(){return cats;}
 public <T> T getParcelableExtra(String k,Class<T> t){return t.cast(extra);}
 public CharSequence getCharSequenceExtra(String k){return explanation;}
 public int getFlags(){return flags;}
 }""",
'android/content/IntentFilter.java': 'package android.content; public class IntentFilter { public static final int MATCH_CATEGORY_EMPTY=0x100000,MATCH_ADJUSTMENT_NORMAL=0x8000; public IntentFilter(String a){} public void addCategory(String a){} public void setPriority(int p){} }',
'android/content/pm/ApplicationInfo.java': 'package android.content.pm; public class ApplicationInfo { public static final int FLAG_SYSTEM=1,FLAG_INSTALLED=2; public String packageName; public int uid,flags,targetSdkVersion; public boolean enabled; }',
'android/content/pm/ActivityInfo.java': 'package android.content.pm; public class ActivityInfo { public String packageName,name,processName; public ApplicationInfo applicationInfo; public boolean exported,enabled,directBootAware; public CharSequence nonLocalizedLabel; }',
'android/content/pm/PackageManager.java': 'package android.content.pm; public class PackageManager { public static final int MATCH_DIRECT_BOOT_AWARE=1,MATCH_DIRECT_BOOT_UNAWARE=2,GET_RESOLVED_FILTER=4; }',
'android/content/pm/ResolveInfo.java': 'package android.content.pm; public class ResolveInfo { public ActivityInfo activityInfo; public android.os.UserHandle userHandle; public int priority,match; public boolean isDefault,system; public android.content.IntentFilter filter; }',
'android/os/Binder.java': 'package android.os; public class Binder { public static long clearCallingIdentity(){return 0;} public static void restoreCallingIdentity(long i){} }',
'android/os/Process.java': 'package android.os; public class Process { public static final int SYSTEM_UID=1000; }',
'android/os/Build.java': 'package android.os; public class Build { public static class VERSION { public static final int SDK_INT=36; } }',
'android/os/UserHandle.java': 'package android.os; public class UserHandle { public static final UserHandle SYSTEM=new UserHandle(); }',
'com/gesher/server/SettingsIntents.java': 'package com.gesher.server; class SettingsIntents { static boolean addressesSettings(android.content.Intent i){return i.getComponent()==null && (i.getPackage()==null || i.getPackage().equals("com.android.settings"));} }',
'com/gesher/server/SystemServer.java': 'package com.gesher.server; class SystemServer { static void log(String s){} }',
'com/gesher/server/AccountSecretStore.java': 'package com.gesher.server; class AccountSecretStore { static void ensureLoaded(){} }',
'com/gesher/server/PackageManagerService.java': 'package com.gesher.server; class PackageManagerService { Object installed; final DeviceAdminPolicyService policy=new DeviceAdminPolicyService(); Object get(String name){return installed;} DeviceAdminPolicyService deviceAdminPolicy(){return policy;} }',
'com/gesher/server/DeviceAdminPolicyService.java': """package com.gesher.server; class DeviceAdminPolicyService {
 static class Request { android.content.ComponentName admin; String label="Test app",description="Description"; boolean supported=true; }
 Request request; boolean active,commit=true; int activations;
 Request prepare(android.content.ComponentName admin){return admin==null ? null : request;}
 boolean isAdminActive(android.content.ComponentName c,int u){return active;}
 boolean activate(Request r){activations++;return commit;}
 }""",
'com/gesher/server/EnrollmentQueueTest.java': r"""package com.gesher.server;
import android.app.Activity;
import android.content.*;
import java.util.*;
public class EnrollmentQueueTest {
 static int checks;
 static void check(boolean yes,String why){if(!yes)throw new AssertionError(why);checks++;}
 static Intent intent(){Intent i=new Intent();i.action=DeviceAdminRequests.ACTION;i.extra=new ComponentName("fixture.pkg","AdminReceiver");return i;}
 public static void main(String[] args){
  PackageManagerService pm=new PackageManagerService();
  DeviceAdminPolicyService.Request r=new DeviceAdminPolicyService.Request();r.admin=(ComponentName)intent().extra;pm.policy.request=r;
  ArrayDeque<Runnable> queue=new ArrayDeque<>(); List<Integer> results=new ArrayList<>();
  int[] shown={0},before={0}; boolean[] decision={false}; String[] description={null};
  DeviceAdminRequests requests=new DeviceAdminRequests(pm,queue::add,(label,desc,supported)->{shown[0]++;description[0]=desc;check(label.contains("fixture.pkg/AdminReceiver"),"stable identity shown");return decision[0];});
  check(DeviceAdminRequests.handles(intent(),null),"implicit enrollment resolves");
  Intent wrong=intent();wrong.type="text/plain";check(!DeviceAdminRequests.handles(wrong,wrong.type),"MIME rejected");
  wrong=intent();wrong.data="file:x";check(!DeviceAdminRequests.handles(wrong,null),"data rejected");
  wrong=intent();wrong.pkg="other.settings";check(!DeviceAdminRequests.handles(wrong,null),"unrelated package rejected");
  wrong=intent();wrong.cats=Set.of("unknown");check(!DeviceAdminRequests.handles(wrong,null),"unknown category rejected");
  check(requests.resolve(intent(),null,4).filter!=null,"resolved filter available");
  pm.installed=new Object();check(requests.resolve(intent(),null,0)==null,"installed Settings takes precedence");pm.installed=null;
  requests.request(intent(),()->before[0]++,results::add);
  check(results.isEmpty() && shown[0]==0,"no early completion before queued window");queue.remove().run();
  check(results.remove(0)==Activity.RESULT_CANCELED && pm.policy.activations==0,"deny never activates");
  decision[0]=true;requests.request(intent(),()->before[0]++,results::add);queue.remove().run();
  check(results.remove(0)==Activity.RESULT_OK && pm.policy.activations==1,"explicit consent commits before success");
  pm.policy.commit=false;requests.request(intent(),()->{},results::add);queue.remove().run();
  check(results.remove(0)==Activity.RESULT_CANCELED,"stale installation commit fails closed");
  r.supported=false;pm.policy.active=true;int previous=pm.policy.activations;
  requests.request(intent(),()->{},results::add);queue.remove().run();
  check(results.remove(0)==Activity.RESULT_CANCELED && pm.policy.activations==previous,"unsupported updated active administrator never succeeds");
  r.supported=true;pm.policy.active=true;previous=shown[0];requests.request(intent(),()->{},results::add);queue.remove().run();
  check(results.remove(0)==Activity.RESULT_OK && shown[0]==previous,"already active supported administrator needs no second consent");
  pm.policy.active=false;pm.policy.request=null;requests.request(intent(),()->{},results::add);queue.remove().run();
  check(results.remove(0)==Activity.RESULT_CANCELED,"invalid receiver cancels cleanly");
  Intent malformed=intent();malformed.extra="wrong type";requests.request(malformed,()->{},results::add);
  check(results.remove(0)==Activity.RESULT_CANCELED,"malformed extra cancels cleanly");
  pm.policy.request=r;pm.policy.active=false;decision[0]=false;
  Intent outer=intent();outer.selector=new Intent();outer.selector.action=DeviceAdminRequests.ACTION;
  outer.explanation="Reason from app {0}";
  check(DeviceAdminRequests.handles(outer,null),"selector resolves enrollment without replacing original extras");
  requests.request(outer,()->{},results::add);queue.remove().run();
  check(results.remove(0)==Activity.RESULT_CANCELED && description[0].contains("Reason from app {0}"),"original explanation remains literal beside receiver description");
  outer.explanation="x".repeat(5000);
  requests.request(outer,()->{},results::add);queue.remove().run();results.clear();
  check(description[0].length()<4200 && description[0].endsWith("\u2026"),"app explanation is bounded and marked truncated");
  decision[0]=true;
  for(boolean active:new boolean[]{false,true}) {
   pm.policy.active=active;
   for(boolean selector:new boolean[]{false,true}) {
    Intent owner=intent();owner.action="android.app.action.SET_PROFILE_OWNER";
    if(selector){owner.selector=new Intent();owner.selector.action=DeviceAdminRequests.ACTION;}
    else owner.component=new ComponentName(DeviceAdminRequests.PACKAGE,DeviceAdminRequests.ACTIVITY);
    check(DeviceAdminRequests.handles(owner,null),"unsupported operation can address the Settings target");
    int dialogsBefore=shown[0],activationsBefore=pm.policy.activations;
    check(requests.request(owner,()->{throw new AssertionError("owner action must not open legacy UI");},results::add),"unsupported original action completes safely");
    check(results.remove(0)==Activity.RESULT_CANCELED && queue.isEmpty()
      && shown[0]==dialogsBefore && pm.policy.activations==activationsBefore,
      "explicit or selected owner provisioning never becomes successful legacy enrollment, even if active");
   }
  }
  Intent noAction=intent();noAction.action=null;noAction.component=new ComponentName(DeviceAdminRequests.PACKAGE,DeviceAdminRequests.ACTIVITY);
  requests.request(noAction,()->{},results::add);
  check(results.remove(0)==Activity.RESULT_CANCELED && queue.isEmpty(),"missing original enrollment action never grants administration");
  for(boolean active:new boolean[]{false,true}) {
   pm.policy.active=active;
   Intent unparented=intent();unparented.flags=Intent.FLAG_ACTIVITY_NEW_TASK;
   int dialogsBefore=shown[0],activationsBefore=pm.policy.activations;
   requests.request(unparented,()->{throw new AssertionError("NEW_TASK enrollment must not open UI");},results::add);
   check(results.remove(0)==Activity.RESULT_CANCELED && queue.isEmpty()
     && shown[0]==dialogsBefore && pm.policy.activations==activationsBefore,
     "NEW_TASK enrollment cancels without changing authority even if already active");
  }
  pm.policy.active=false;decision[0]=false;
  pm.policy.request=r;for(int i=0;i<16;i++)check(requests.request(intent(),()->{},results::add),"bounded admission");
  check(!requests.request(intent(),()->{},results::add),"17th concurrent request rejected");
  while(!queue.isEmpty())queue.remove().run();
  check(requests.request(intent(),()->{},results::add),"completion releases queue slots");queue.remove().run();
  check(before[0]==2,"pause barrier precedes dialogs");
  System.out.println("device admin request queue: "+checks+" checks passed; explicit service/framework/host boundaries");
 }
}"""
}
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home',type=Path,required=True)
    args=parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='gesher-admin-requests-') as temporary:
        work=Path(temporary); sources=[]
        for name,source in STUBS.items():
            path=work/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(source,encoding='utf8');sources.append(str(path))
        sources.append(str(ROOT/'app/java/com/gesher/server/DeviceAdminRequests.java'))
        subprocess.run([str(args.java_home/'bin/javac'),'--release','17','-encoding','UTF-8','-d',str(work),*sources],check=True,timeout=60)
        subprocess.run([str(args.java_home/'bin/java'),'-cp',str(work),'com.gesher.server.EnrollmentQueueTest'],check=True,timeout=30)
if __name__=='__main__': main()
