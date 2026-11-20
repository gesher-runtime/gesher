/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
// Host-only declared Binder/Process/compatibility boundary; never packaged in Gesher.
package gesher.services;
import android.app.role.RoleManager;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.pm.PackageManager;
import android.os.Binder;
import android.os.UserHandle;
import android.app.compat.CompatChanges;
import java.util.ArrayDeque;
import java.util.Set;
public final class IndependentReqa {
 static int passed,failed;
 static void check(boolean v,String m){if(v){passed++;}else{failed++;System.out.println("FAIL: "+m);}}
 static class C extends ContextWrapper{
  int user; Set<String> allowed;
  C(int u,String... p){super(null);user=u;allowed=Set.of(p);}
  public int getUserId(){return user;}
  public int checkSelfPermission(String p){return allowed.contains(p)?PackageManager.PERMISSION_GRANTED:PackageManager.PERMISSION_DENIED;}
  public int checkCallingOrSelfPermission(String p){return allowed.contains(p)?PackageManager.PERMISSION_GRANTED:PackageManager.PERMISSION_DENIED;}
  public void enforceCallingOrSelfPermission(String p,String m){if(checkCallingOrSelfPermission(p)!=0)throw new SecurityException(p);}
 }
 static RoleManager m(C c)throws Exception{var k=RoleManager.class.getDeclaredConstructor(Context.class);k.setAccessible(true);return k.newInstance(c);}
 static void expectSecurity(Runnable r,String label){try{r.run();check(false,label);}catch(SecurityException good){passed++;}}
 public static void main(String[] a)throws Exception{
  RoleManagerUnsupportedTest.contractChecks();
  var f=RoleManagerUnsupportedTest.class.getDeclaredField("checks");f.setAccessible(true);
  System.out.println("Reproduced existing production fixture checks: "+f.getInt(null));
  RoleManager cross=m(new C(10,"android.permission.MANAGE_ROLE_HOLDERS"));
  CompatChanges.modern=true;
  expectSecurity(()->cross.isRoleAvailable(RoleManager.ROLE_SMS),"modern availability must enforce context user");
  expectSecurity(()->cross.isRoleHeld(RoleManager.ROLE_SMS),"modern held query must enforce context user");
  expectSecurity(()->cross.getRoleHolders(RoleManager.ROLE_SMS),"modern holder query must enforce context user");
  CompatChanges.modern=false;
  check(!cross.isRoleAvailable(RoleManager.ROLE_SMS),"legacy availability uses process user");
  check(!cross.isRoleHeld(RoleManager.ROLE_SMS),"legacy held uses process user");
  check(cross.getRoleHolders(RoleManager.ROLE_SMS).isEmpty(),"legacy holder query uses process user");
  CompatChanges.modern=true;
  RoleManager r=m(new C(0,"android.permission.MANAGE_ROLE_HOLDERS"));
  ArrayDeque<Runnable> q=new ArrayDeque<>();
  Binder.uid=10001;
  int[] seen={0};
  r.clearRoleHoldersAsUser(RoleManager.ROLE_SMS,0,UserHandle.of(0),q::add,result->seen[0]=Binder.getCallingUid());
  check(seen[0]==0,"callback is deferred");
  Binder.uid=210001;
  q.remove().run();
  check(seen[0]==10001,"deferred callback must clear executing thread Binder identity; saw "+seen[0]);
  check(Binder.getCallingUid()==210001,"callback must restore executing thread identity");
  Binder.uid=10001;
  r.clearRoleHoldersAsUser(RoleManager.ROLE_SMS,0,UserHandle.of(0),q::add,result->{seen[0]=Binder.getCallingUid();throw new IllegalStateException("owned probe");});
  Binder.uid=310001;
  try{q.remove().run();check(false,"callback failure must propagate");}catch(IllegalStateException good){passed++;}
  check(seen[0]==10001,"throwing deferred callback must clear executing identity; saw "+seen[0]);
  check(Binder.getCallingUid()==310001,"throwing callback restores identity");
  System.out.println("Independent additions: "+passed+" passed, "+failed+" failed");
  if(failed!=0)System.exit(1);
 }
}
