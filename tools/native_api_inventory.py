#!/usr/bin/env python3
"""Compare verified NDK stub exports with lookup results in isolated guest VMs.

Presence only: no resolved API function pointer is invoked. RTLD_DEFAULT and handle lookup
can share Gesher's global resolver; neither establishes per-library namespaces,
signature correctness, behavior, callback completion or hardware availability.
"""
import argparse
import collections
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def parse_exports(text):
    rows = {}
    for line in text.splitlines():
        p = line.split()
        if len(p) < 8 or not p[0].endswith(':') or p[6] == 'UND':
            continue
        if p[4] not in ('GLOBAL','WEAK','GNU_UNIQUE') or p[5] not in ('DEFAULT','PROTECTED') or p[3] in ('SECTION','FILE'):
            continue
        symbol = p[7].split('@',1)[0]
        if not re.fullmatch(r'[A-Za-z_$][A-Za-z0-9_.$]*',symbol):
            raise ValueError('Unexpected export identifier')
        rows[symbol] = {'symbol':symbol,'binding':p[4],'type':p[3],'versioned_name':p[7]}
    return list(rows.values())


def exports(readelf, path):
    text = subprocess.run([str(readelf), '--dyn-syms', '-W', str(path)], capture_output=True,
                          text=True, check=True).stdout
    return parse_exports(text)


def loaded_translator(pid):
    """Read the module list of our own disposable child; never attach/stop it."""
    if os.name!='nt':return None
    import ctypes as c
    from ctypes import wintypes as w
    class Module(c.Structure):
        _fields_=[('size',w.DWORD),('module_id',w.DWORD),('pid',w.DWORD),('global_uses',w.DWORD),
                  ('process_uses',w.DWORD),('base',c.c_void_p),('base_size',w.DWORD),('handle',c.c_void_p),
                  ('name',w.WCHAR*256),('path',w.WCHAR*260)]
    k=c.WinDLL('kernel32',use_last_error=True)
    k.CreateToolhelp32Snapshot.argtypes=[w.DWORD,w.DWORD];k.CreateToolhelp32Snapshot.restype=c.c_void_p
    k.Module32FirstW.argtypes=[c.c_void_p,c.POINTER(Module)];k.Module32FirstW.restype=w.BOOL
    k.Module32NextW.argtypes=[c.c_void_p,c.POINTER(Module)];k.Module32NextW.restype=w.BOOL
    k.CloseHandle.argtypes=[c.c_void_p];k.CloseHandle.restype=w.BOOL
    handle=k.CreateToolhelp32Snapshot(0x18,pid)
    if handle in (None,c.c_void_p(-1).value):return None
    try:
        module=Module();module.size=c.sizeof(module)
        ok=k.Module32FirstW(handle,c.byref(module))
        while ok:
            if module.name.lower()=='gesher_arm64.dll':
                path=Path(module.path)
                return {'path':str(path),'sha256':sha(path),'bytes':path.stat().st_size,'verified_from_child_module_list':True}
            ok=k.Module32NextW(handle,c.byref(module))
    finally:k.CloseHandle(handle)
    return None


def android(path):
    return str(path.resolve()).replace('\\','/')[2:]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--reference-dir',type=Path,required=True)
    ap.add_argument('--runtime',type=Path,required=True)
    ap.add_argument('--boot-dir',type=Path,required=True)
    ap.add_argument('--ndk-bin',type=Path,required=True)
    ap.add_argument('--d8-jar',type=Path)
    ap.add_argument('--java',default='java')
    ap.add_argument('--javac',default='javac')
    ap.add_argument('--runtime-source-revision',help='Caller-supplied build provenance; binary hashes are independently recorded.')
    ap.add_argument('--boot-image',type=Path,help='Optional explicit image; otherwise use the VM default with hashed boot jars.')
    ap.add_argument('--out-dir',type=Path,default=Path('out/compat-audit/native-api36'))
    args=ap.parse_args()
    root=Path(__file__).resolve().parents[1]
    out=args.out_dir.resolve();out.mkdir(parents=True,exist_ok=True)
    reference=json.loads((args.reference_dir/'reference-manifest.json').read_text())
    if reference.get('api')!=36 or reference.get('full_archive_sha1_verified') is not True:
        raise ValueError('Verified API36 reference required')
    classes=out/'classes';classes.mkdir(exist_ok=True)
    subprocess.run([args.javac,'--release','11','-d',str(classes),str(root/'probes/native-api/ApiExportInventory.java')],check=True)
    d8=args.d8_jar
    if d8 is None:
        available=sorted((Path.home()/'tools/android-sdk/build-tools').glob('*/lib/d8.jar'),key=lambda p:tuple(int(x) if x.isdigit() else x for x in re.split(r'[.-]',p.parents[1].name)))
        if not available:raise ValueError('No d8.jar found; pass --d8-jar')
        d8=available[-1]
    dex=out/'native-api-probe.jar'
    subprocess.run([args.java,'-cp',str(d8),'com.android.tools.r8.D8','--release','--min-api','26','--output',str(dex),str(classes/'gesher/ApiExportInventory.class')],check=True)
    readelf=args.ndk_bin/'llvm-readelf.exe'
    rows=[];summaries=[];runs=[];skipped=[]
    for abi,triple in [('x86_64','x86_64-linux-android'),('arm64-v8a','aarch64-linux-android')]:
        lib=out/('api-probe-'+abi+'.so')
        subprocess.run([str(args.ndk_bin/'clang.exe'),'--target='+triple+'29','-shared','-fPIC','-O2','-fno-stack-protector','-Wall','-Wextra','-Werror','-Wl,-soname,libapi_probe.so','-o',str(lib),str(root/'probes/native-api/api_export_probe.c'),'-ldl'],check=True)
        expected=[]
        for item in reference['files']:
            if item['abi']!=abi:continue
            path=args.reference_dir/abi/item['library']
            if sha(path)!=item['sha256']:raise ValueError('Reference file hash mismatch')
            if path.read_bytes()[:4]!=b'\x7fELF':
                skipped.append({'abi':abi,'library':item['library'],'reason':'NDK linker script, not an ELF export stub','sha256':item['sha256']});continue
            expected.extend({'abi':abi,'library':item['library'],'reference_sha256':item['sha256'],**entry} for entry in exports(readelf,path))
        expected.sort(key=lambda r:(r['library'],r['symbol']))
        manifest=out/('expected-'+abi+'.tsv')
        manifest.write_text(''.join(r['library']+'\t'+r['symbol']+'\n' for r in expected),encoding='ascii')
        result=out/('lookups-'+abi+'.tsv')
        data=out/'art-data'/abi;data.mkdir(parents=True,exist_ok=True)
        profile=Path(os.environ['LOCALAPPDATA'])/'Gesher'
        env=os.environ.copy();env.pop('GESHER_BINDER',None)
        env.update(ANDROID_ROOT=android(profile/'system'),ANDROID_DATA=android(data),ANDROID_ART_ROOT=android(profile/'art'),ANDROID_I18N_ROOT=android(profile/'i18n'),ANDROID_TZDATA_ROOT=android(args.boot_dir/'tzdata'))
        bcp=':'.join(android(args.boot_dir/(n+'.jar')) for n in ['core-oj','core-libart','core-icu4j','framework','gesher-app'])
        image_args=['-Ximage:'+android(args.boot_image)] if args.boot_image else []
        command=[str(args.runtime.resolve()),'-Xbootclasspath:'+bcp,*image_args,'-Xint','-Xusejit:false','-cp',android(dex),'gesher.ApiExportInventory',android(lib),android(manifest),android(result),'1000']
        child=subprocess.Popen(command,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        bridge=None
        if abi=='arm64-v8a':
            deadline=time.monotonic()+10
            while child.poll() is None and time.monotonic()<deadline:
                bridge=loaded_translator(child.pid)
                if bridge:break
                time.sleep(0.05)
        try:raw=child.communicate(timeout=120)[0]
        except subprocess.TimeoutExpired:
            child.kill();child.communicate(timeout=10);raise RuntimeError('Isolated native inventory deadline exceeded')
        (out/('run-'+abi+'.log')).write_bytes(raw)
        if child.returncode or ('Native API inventory complete: '+str(len(expected))).encode() not in raw:
            raise RuntimeError('Isolated native inventory failed for '+abi+'; inspect fixture log')
        if abi=='arm64-v8a' and bridge is None:raise RuntimeError('Could not verify translator module in isolated ARM64 fixture')
        actual={};opened={};ended=None
        for line in result.read_text().splitlines():
            p=line.split('\t')
            if p[0]=='OPEN':opened[p[1]]=p[2]=='1'
            elif p[0]=='SYMBOL':actual[(p[1],p[2])]=(p[3]=='1',p[4]=='1')
            elif p[0]=='END':ended=int(p[1])
        if len(actual)!=len(expected) or ended!=len(expected):raise RuntimeError('Incomplete lookup output')
        for row in expected:
            global_found,handle_found=actual[(row['library'],row['symbol'])]
            row.update(global_found=global_found,library_handle_opened=opened[row['library']],handle_found=handle_found,
                       status='symbol_present_unverified' if global_found else 'missing_lookup_symbol')
            if row['symbol']=='__libc_init':row['scope_note']='Executable startup/gesher-exec scope; shared-VM absence is not by itself an executable-launch failure.'
            rows.append(row)
        for library in sorted(opened):
            group=[r for r in expected if r['library']==library]
            summaries.append({'abi':abi,'library':library,'checked':len(group),'present':sum(r['global_found'] for r in group),'missing':sum(not r['global_found'] for r in group),'handle_opened':opened[library]})
        runs.append({'abi':abi,'fixture_pid':child.pid,'exit_code':child.returncode,'probe_sha256':sha(lib),'lookups_sha256':sha(result),'expected_symbols':len(expected),'translator':bridge,'shared_binder_endpoint_removed':True})
        print(json.dumps({'abi':abi,'checked':len(expected),'present':sum(r['global_found'] for r in expected),'missing':sum(not r['global_found'] for r in expected)}),flush=True)
    report={'schema_version':1,'api_level':36,'scope':'Public NDK export-name lookup only; no resolved API function pointer invoked. Not semantic or library namespace conformance.',
            'runtime':{'path':str(args.runtime.resolve()),'sha256':sha(args.runtime),'caller_supplied_source_revision':args.runtime_source_revision},'reference':reference,'runs':runs,'libraries':summaries,'skipped':skipped,'symbols':rows,
            'boot_jars':[{'name':name+'.jar','sha256':sha(args.boot_dir/(name+'.jar'))} for name in ['core-oj','core-libart','core-icu4j','framework','gesher-app']],
            'boot_image_entry':{'path':str(args.boot_image),'sha256':sha(args.boot_image)} if args.boot_image else None,
            'boot_image_policy':'Explicit image recorded when provided; otherwise VM default with -Xint/-Xusejit:false and hashed boot jars.',
            'source_inputs':{str(path.relative_to(root)):sha(path) for path in [root/'probes/native-api/ApiExportInventory.java',root/'probes/native-api/api_export_probe.c']},
            'tools':{'d8_jar':str(d8),'d8_sha256':sha(d8),'java':args.java,'javac':args.javac,'readelf':str(readelf),'readelf_sha256':sha(readelf)},
            'limits':['A found symbol does not validate arguments, ABI details, return values, callback completion or hardware.',
                      'Gesher platform handles and RTLD_DEFAULT can use the same global resolver; handle lookup is not proof of per-library namespaces.',
                      'ARM64 runs the probe as an ARM64 ELF guest in this x86_64 Windows runtime; it is not a native ARM64 Windows host build.',
                      'Only API36 stub exports are compared; APK private/third-party imports and other API levels are not included.',
                      'Symbol version names are recorded, but dlsym name presence does not prove versioned-symbol resolution.'],
            'generator_sha256':sha(Path(__file__))}
    (out/'native-api36-presence.json').write_text(json.dumps(report,indent=2)+'\n')
    lines=['# API36 native export-name lookup','','Presence only, not behavior or per-library namespace conformance.','','| ABI | Library | Checked | Present | Missing | Handle opened |','| --- | --- | ---: | ---: | ---: | --- |']
    lines+=['| {abi} | {library} | {checked} | {present} | {missing} | {handle_opened} |'.format(**r) for r in summaries]
    (out/'native-api36-presence.md').write_text('\n'.join(lines)+'\n')


if __name__=='__main__':main()
