/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.fixture;
import android.app.ActivityManager;
import android.content.Intent;
public final class PackageEventsControl {
    public static void main(String[] args)throws Exception{
        if(args.length!=1 || !(args[0].equals("quiet1")||args[0].equals("quiet2")||args[0].equals("finish")
                ||args[0].equals("baseline-warm")||args[0].equals("baseline-finish")
                ||args[0].equals("probe1")||args[0].equals("probe2")))
            throw new IllegalArgumentException("Pass a fixture control enum");
        System.loadLibrary("android_runtime");
        if(args[0].startsWith("probe")){
            android.content.pm.IPackageManager pm=android.content.pm.IPackageManager.Stub.asInterface(
                    android.os.ServiceManager.getService("package"));
            android.content.pm.PackageInfo p=pm.getPackageInfo("com.gesher.packageevents.target",0,0);
            int expected=args[0].equals("probe1")?1:2;
            if(p==null||p.getLongVersionCode()!=expected)throw new AssertionError("Target not durably installed");
            System.out.println("packageevents-control: target-installed version="+expected);return;
        }
        ActivityManager.getService().broadcastIntentWithFeature(null,null,new Intent("com.gesher.packageevents.CONTROL")
                .setPackage("com.gesher.packageevents.observer").putExtra("command",args[0]),
                null,null,0,null,null,null,null,null,-1,null,false,false,0);
        System.out.println("packageevents-control: submitted "+args[0]);
    }
}
