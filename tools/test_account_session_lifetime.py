#!/usr/bin/env python3
"""Compile the whole production account service and test Answer lifetimes/deadlines.

The fixture supplies synthetic authenticator metadata and existing host JNI
identity/allocation boundaries; no production Java method is replaced.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('java-home', 'framework-classes', 'core-classes', 'app-classes', 'apksig-jar'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--cc', default=shutil.which('cc'))
    parser.add_argument('--source-ref')
    args = parser.parse_args()
    if not args.cc:
        parser.error('C compiler required (--cc)')
    cp = os.pathsep.join(str(p.resolve()) for p in
                         (args.app_classes,args.framework_classes,args.core_classes,args.apksig_jar))
    with tempfile.TemporaryDirectory(prefix='gesher-account-lifetime-') as tmp:
        work=Path(tmp); classes=work/'classes'; classes.mkdir()
        windows=os.name=='nt'; native=work/('account_host.dll' if windows else 'libaccount_host.so')
        subprocess.run([args.cc,'-std=c11','-D_POSIX_C_SOURCE=200809L','-shared',
                        *([] if windows else ['-fPIC']), '-I'+str(args.java_home/'include'),
                        '-I'+str(args.java_home/'include'/('win32' if windows else 'linux')),
                        str(ROOT/'tests/account-session/framework_host.c'),'-o',str(native),
                        *(['-static','-lwinpthread'] if windows else [])],check=True)
        relative='app/java/com/gesher/server/AccountManagerService.java'
        source=ROOT/relative
        if args.source_ref:
            source=work/'AccountManagerService.java'
            source.write_bytes(subprocess.run(['git','show',args.source_ref+':'+relative],cwd=ROOT,
                                              check=True,capture_output=True).stdout)
        subprocess.run([str(args.java_home/'bin/javac'),'-J-XX:-UsePerfData','-encoding','UTF-8',
                        '-cp',cp,'-d',str(classes),str(source),
                        str(ROOT/'tests/account-session/AccountSessionLifetimeHost.java'),
                        str(ROOT/'tests/account-session/AccountSessionTimeoutHost.java'),
                        str(ROOT/'tests/account-session/AccountRetirementHost.java'),
                        str(ROOT/'tests/account-session/AccountExplicitRemovalRaceHost.java'),
                        str(ROOT/'tests/account-session/AccountRetirementLockHost.java')],check=True)
        cleaner=work/'cleaner/sun/misc'; cleaner.mkdir(parents=True)
        for entry in (args.core_classes/'sun/misc').glob('Cleaner*.class'):
            shutil.copy2(entry,cleaner/entry.name)
        env=os.environ.copy(); env['ANDROID_DATA']=str(work/'data'); env['GESHER_MICROG_SPOOF']='0'
        for fixture, arguments in (('AccountSessionLifetimeHost', ()),
                ('AccountSessionTimeoutHost', ()), ('AccountRetirementHost', ()),
                ('AccountExplicitRemovalRaceHost', (str(work/'cleaner'),)),
                ('AccountRetirementLockHost', ('false',)), ('AccountRetirementLockHost', ('true',))):
            if fixture == 'AccountRetirementLockHost': env['GESHER_MICROG_SPOOF']='1'
            subprocess.run([str(args.java_home/'bin/java'),'-XX:-UsePerfData',
                            '--add-opens','java.base/java.util.concurrent=ALL-UNNAMED',
                            '--patch-module','jdk.unsupported='+str(work/'cleaner'),
                            '-cp',str(classes)+os.pathsep+cp,fixture,str(native),*arguments],
                           check=True,env=env,timeout=45)

if __name__=='__main__':
    main()
