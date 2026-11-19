#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
"""Genuine DNS-SD record/layout/scheduler contracts with declared host I/O boundaries."""
import argparse, hashlib, json, os, shlex, subprocess, time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def digest(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('java-home','app-classes','framework-classes','core-classes','output'):
        p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--old-manager-source',type=Path)
    a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    if not (a.core_classes/'dalvik/system/VMRuntime.class').is_file():
        raise ValueError('The actual Android core class root must contain dalvik/system/VMRuntime.class')
    initial_sources=list((ROOT/'app/java').rglob('*.java'))+list((ROOT/'tests/nsd').glob('Nsd*Registration*.java'))
    initial_sources+=list((ROOT/'tests/nsd').glob('NsdAdvertiser*.java'))+[Path(__file__),ROOT/'tests/nsd/registration_os_constants_host.cc']
    (out/'initial-source-hashes.json').write_text(json.dumps({str(x):digest(x) for x in initial_sources},indent=2)+'\n')
    (out/'initial-class-hashes.json').write_text(json.dumps({str(x):digest(x) for x in a.app_classes.rglob('*.class')},indent=2)+'\n')
    commands=[];observations=[]
    def run(argv,label,expected_failure=None):
        commands.append(argv);(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
        started=time.monotonic();r=subprocess.run(argv,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=90)
        (out/(label+'.log')).write_text(r.stdout);print(r.stdout,end='')
        observations.append({'label':label,'exit':r.returncode,'seconds':time.monotonic()-started})
        (out/'observations.json').write_text(json.dumps(observations,indent=2)+'\n')
        if expected_failure is not None:
            if not r.returncode or expected_failure not in r.stdout:raise RuntimeError(label+' did not fail at the intended old behavior')
        elif r.returncode:raise RuntimeError(label+' failed')
    lib=out/'libnsd_registration_host.so';dep=out/'native.d'
    run(['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-shared','-fPIC','-MMD','-MF',str(dep),
         '-I'+str(a.java_home/'include'),'-I'+str(a.java_home/'include/linux'),str(ROOT/'tests/nsd/framework_host.c'),'-o',str(lib)],'native')
    constants=ROOT/'tests/nsd/registration_os_constants_host.cc';constants_lib=out/'libnsd_os_constants_host.so'
    constants_dep=out/'os-constants-native.d'
    run(['c++','-std=c++17','-shared','-fPIC','-MD','-MF',str(constants_dep),
         '-I'+str(a.java_home/'include'),'-I'+str(a.java_home/'include/linux'),str(constants),'-o',str(constants_lib)],'os-constants-native')
    cp=os.pathsep.join(map(str,(a.app_classes,a.framework_classes,a.core_classes)))
    fixture=ROOT/'tests/nsd/NsdAdvertiserContractTest.java';manager=ROOT/'tests/nsd/NsdRegistrationManagerTest.java';classes=out/'classes'
    protocol=ROOT/'tests/nsd/NsdAdvertiserProtocolTest.java'
    retirement=ROOT/'tests/nsd/NsdAdvertiserRetirementTest.java'
    run([str(a.java_home/'bin/javac'),'-J-Xmx700m','-encoding','UTF-8','-cp',cp,'-d',str(classes),str(fixture),str(manager),str(protocol),str(retirement)],'compile-fixture')
    run([str(a.java_home/'bin/java'),'-XX:-UsePerfData','-XX:+UnlockDiagnosticVMOptions','-XX:-BytecodeVerificationRemote',
         '-cp',str(classes)+os.pathsep+cp,'com.gesher.net.mdns.NsdAdvertiserContractTest',str(lib),str(out/'wire'),
         a.app_classes.resolve().as_uri()+'/',str(constants_lib)],'contracts')
    java=[str(a.java_home/'bin/java'),'-XX:-UsePerfData','-XX:+UnlockDiagnosticVMOptions','-XX:-BytecodeVerificationRemote']
    run(java+['-cp',str(classes)+os.pathsep+cp,'com.gesher.net.NsdRegistrationManagerTest',str(lib)],'manager-contracts')
    if a.old_manager_source:
        old=out/'old-manager-classes'
        run([str(a.java_home/'bin/javac'),'-J-Xmx700m','-encoding','UTF-8','-cp',cp,'-d',str(old),str(a.old_manager_source)],'compile-old-manager')
        run(java+['-cp',str(classes)+os.pathsep+str(old)+os.pathsep+cp,'com.gesher.net.NsdRegistrationManagerTest',str(lib),'probe'],
            'old-manager-negative','observed=[failed:0]')
    includes=[Path(v).resolve() for v in shlex.split(dep.read_text().replace('\\\n','').partition(':')[2])]
    includes+= [Path(v).resolve() for v in shlex.split(constants_dep.read_text().replace('\\\n','').partition(':')[2])]
    sources=[fixture,manager,protocol,retirement,constants,Path(__file__)]+list((ROOT/'app/java/com/gesher/net/mdns').glob('*.java'))
    sources += [ROOT/'app/java/com/gesher/net/WindowsNsdAdvertiser.java',ROOT/'app/java/com/gesher/net/NsdRegistrationContract.java',ROOT/'app/java/android/net/nsd/NsdServiceInfo.java']
    sources += [ROOT/'app/java'/p for p in ('android/net/nsd/NsdManager.java','android/net/nsd/AdvertisingRequest.java','com/gesher/net/NsdRegistrationClient.java','com/gesher/net/INsdRegistration.java')]
    providerfiles=list((a.app_classes/'com/gesher/net/mdns').glob('*.class'))+list((a.app_classes/'com/gesher/net').glob('WindowsNsdAdvertiser*.class'))
    providerfiles+=list((a.app_classes/'com/gesher/net').glob('NsdRegistrationContract*.class'))+list((a.app_classes/'android/net/nsd').glob('NsdServiceInfo*.class'))
    for folder,pattern in [('android/net/nsd','NsdManager*.class'),('android/net/nsd','AdvertisingRequest*.class'),('com/gesher/net','NsdRegistrationClient*.class'),('com/gesher/net','INsdRegistration*.class')]:
        providerfiles+=list((a.app_classes/folder).glob(pattern))
    manifest={'commands':commands,'observations':observations,'sourceHashes':{str(x):digest(x) for x in sources},
              'classHashes':{str(x):digest(x) for x in providerfiles},'fixtureClasses':{str(x):digest(x) for x in classes.rglob('*.class')},
              'nativeIncludes':{str(x):digest(x) for x in includes},'nativeLibrarySha256':digest(lib),
              'osConstantsLibrarySha256':digest(constants_lib),
              'osConstantsProviders':{str(a.core_classes/'android/system'/n):digest(a.core_classes/'android/system'/n) for n in ('OsConstants.class','OsConstantsHolder.class')},
              'wireHashes':{str(x):digest(x) for x in (out/'wire').glob('*')},
              'windowsExecution':False,'nativeSocketExecution':False,'boundaries':['clock','driver topology and I/O','host initializer','complete genuine OsConstantsHolder native initializer compiled against actual Linux headers']}
    if a.old_manager_source:
        manifest['oldManagerSource']={str(a.old_manager_source):digest(a.old_manager_source)}
        manifest['oldManagerClasses']={str(x):digest(x) for x in (out/'old-manager-classes').rglob('*.class')}
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
if __name__=='__main__':main()
