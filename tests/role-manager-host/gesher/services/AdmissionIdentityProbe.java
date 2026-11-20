/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
// Host-only declared Binder/Process/compatibility boundary; never packaged in Gesher.
package gesher.services;
import android.app.role.RoleManager;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.pm.PackageManager;
import android.os.Binder;
import java.util.Set;
public final class AdmissionIdentityProbe {
 static int passed,failed;
 static class C extends ContextWrapper {
  final Set<String> self,remote;
  C(Set<String> s,Set<String> r){super(null);self=s;remote=r;}
  public int getUserId(){return 0;}
  public int checkSelfPermission(String p){return self.contains(p)?0:-1;}
  public int checkCallingOrSelfPermission(String p){return (Binder.getCallingUid()==10001?self:remote).contains(p)?0:-1;}
  public void enforceCallingOrSelfPermission(String p,String m){if(checkCallingOrSelfPermission(p)!=0)throw new SecurityException(p);}
 }
 static RoleManager manager(C c)throws Exception{var k=RoleManager.class.getDeclaredConstructor(Context.class);k.setAccessible(true);return k.newInstance(c);}
 static void check(boolean v,String m){if(v)passed++;else{failed++;System.out.println("FAIL: "+m);}}
 static void test(Runnable r,String m){try{r.run();passed++;}catch(Throwable e){failed++;System.out.println("FAIL: "+m+" -> "+e);}}
 public static void main(String[] ignored)throws Exception{
  RoleManager unpriv=manager(new C(Set.of(),Set.of()));
  Binder.uid=210001;
  test(()->{if(unpriv.isRoleAvailable(RoleManager.ROLE_SMS))throw new AssertionError();},"public availability uses local process user during incoming IPC");
  test(()->{if(unpriv.isRoleHeld(RoleManager.ROLE_SMS))throw new AssertionError();},"public held query uses local process user during incoming IPC");
  Binder.uid=10002;
  RoleManager privileged=manager(new C(Set.of("android.permission.MANAGE_ROLE_HOLDERS"),Set.of()));
  test(()->{if(!privileged.getRoleHolders(RoleManager.ROLE_SMS).isEmpty())throw new AssertionError();},"holder read uses local permission, not unprivileged remote caller");
  RoleManager denied=manager(new C(Set.of(),Set.of("android.permission.MANAGE_ROLE_HOLDERS")));
  try{denied.getRoleHolders(RoleManager.ROLE_SMS);check(false,"remote permission must not authorize local app");}catch(SecurityException good){passed++;}
  check(Binder.uid==10002,"admission preserves caller identity");
  System.out.println("Admission identity checks: "+passed+" passed, "+failed+" failed");
  if(failed>0)System.exit(1);
 }
}
