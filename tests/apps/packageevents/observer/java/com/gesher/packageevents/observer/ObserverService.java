/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.packageevents.observer;
import android.app.Service;
import android.content.*;
import android.content.pm.*;
import android.net.Uri;
import android.os.*;
import android.os.Process;

public final class ObserverService extends Service {
    private static final String TARGET="com.gesher.packageevents.target";
    private final Handler handler=new Handler();
    private int count,stage,uid=-1;private boolean failed;
    private void log(String s){System.out.println("packageevents: "+s);System.out.flush();}
    private void fail(String s){failed=true;log("FAIL "+s);}
    private PackageInfo info() throws Exception { return getPackageManager().getPackageInfo(TARGET,0); }
    private final BroadcastReceiver events=new BroadcastReceiver(){
        @Override public void onReceive(Context context,Intent intent){
            try{
                if(intent.getData()==null || !"package".equals(intent.getScheme())
                        || !TARGET.equals(intent.getData().getSchemeSpecificPart())){fail("uri");return;}
                PackageInfo p=info();long version=p.getLongVersionCode();
                boolean replacing=intent.getBooleanExtra(Intent.EXTRA_REPLACING,false);
                if(intent.getIntExtra(Intent.EXTRA_UID,-1)!=p.applicationInfo.uid){fail("uid");return;}
                if(uid<0)uid=p.applicationInfo.uid;
                if(uid!=p.applicationInfo.uid){fail("uid-changed");return;}
                if(Intent.ACTION_PACKAGE_ADDED.equals(intent.getAction())){
                    if(stage==0 && version==1 && !replacing){stage=1;}
                    else if(stage==1 && version==2 && replacing){stage=2;}
                    else{fail("added-order-cache");return;}
                    ++count;log("ADDED version="+version+" replacing="+replacing+" uid-match=true count="+count);
                }else if(Intent.ACTION_PACKAGE_REPLACED.equals(intent.getAction())){
                    if(stage!=2 || version!=2 || !replacing){fail("replaced-order-cache");return;}
                    stage=3;++count;log("REPLACED version=2 uid-match=true count="+count);
                }else{fail("action");}
                if(info().getLongVersionCode()!=version)fail("repeat-cache");
            }catch(Exception e){fail("package-query");}
        }
    };
    private final BroadcastReceiver control=new BroadcastReceiver(){
        @Override public void onReceive(Context context,Intent intent){
            String command=intent.getStringExtra("command");
            if("baseline-warm".equals(command)){
                try{
                    if(count!=0 || info().getLongVersionCode()!=1 || info().getLongVersionCode()!=1){fail("baseline-warm");return;}
                    log("BASELINE_WARM version=1 count=0");
                }catch(Exception e){fail("baseline-query");}
                return;
            }
            if("baseline-finish".equals(command)){
                try{
                    long observed=info().getLongVersionCode();
                    if(count!=0 || (observed!=1 && observed!=2)){fail("baseline-cache");return;}
                    log("BASELINE EXPECTED no-events=true observed-version="+observed);stopSelf();
                }catch(Exception e){fail("baseline-query");}
                return;
            }
            if(!"quiet1".equals(command) && !"quiet2".equals(command) && !"finish".equals(command)){fail("control");return;}
            final int expected="quiet1".equals(command)?1:3;
            handler.postDelayed(()->{
                try{
                    long expectedVersion=expected==1?1:2;
                    if(failed || count!=expected || info().getLongVersionCode()!=expectedVersion
                            || info().applicationInfo.uid!=uid){fail("quiet-state");return;}
                    log("QUIET command="+command+" count="+count+" version="+expectedVersion);
                    if("finish".equals(command)){log("PASS fresh-update-unchanged-invalid-cache-and-spoof");stopSelf();}
                }catch(Exception e){fail("quiet-query");}
            },1000);
        }
    };
    @Override public void onCreate(){
        super.onCreate();
        IntentFilter f=new IntentFilter();f.addAction(Intent.ACTION_PACKAGE_ADDED);f.addAction(Intent.ACTION_PACKAGE_REPLACED);
        f.addDataScheme("package");f.addDataSchemeSpecificPart(TARGET,PatternMatcher.PATTERN_LITERAL);
        registerReceiver(events,f,null,handler,Context.RECEIVER_EXPORTED);
        registerReceiver(control,new IntentFilter("com.gesher.packageevents.CONTROL"),null,handler,Context.RECEIVER_NOT_EXPORTED);
        try{info();fail("target-not-absent");return;}catch(PackageManager.NameNotFoundException expected){}
        catch(Exception e){fail("warm-query");return;}
        try{info();fail("negative-cache");return;}catch(PackageManager.NameNotFoundException expected){}
        catch(Exception e){fail("warm-query");return;}
        try{
            sendBroadcast(new Intent(Intent.ACTION_PACKAGE_ADDED,Uri.fromParts("package",TARGET,null)));
            fail("ordinary-spoof-accepted");return;
        }catch(SecurityException expected){log("spoof-blocked=true");}
        log("READY absent-cache-warmed=true pid="+Process.myPid());
    }
    @Override public int onStartCommand(Intent intent,int flags,int startId){return START_NOT_STICKY;}
    @Override public IBinder onBind(Intent intent){return null;}
}
