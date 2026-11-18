#!/usr/bin/env python3
"""Exercise actual Bionic state retirement and close/reopen/pclose transactions.

Only the Windows CRT lock/path and descriptor transaction boundaries are host
adapters. Linux performs file opening, closing and reopening; controlled failures
separate pre-close admission, CRT errors after close, and post-close bookkeeping.
This does not claim Windows UCRT slot-reuse or real record-lock fault execution.
"""
import argparse, hashlib, os, pathlib, shlex, subprocess, tempfile
ROOT = pathlib.Path(__file__).resolve().parent.parent

def section(source, first, end):
    start=source.index(first)
    return source[start:source.index(end,start)]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host-source',type=pathlib.Path,default=ROOT/'compat/posix/src/fsops.cc')
    parser.add_argument('--guest-source',type=pathlib.Path,default=ROOT/'compat/bionic/stdio_bionic.cc')
    parser.add_argument('--cxx',default=os.environ.get('CXX','c++'))
    parser.add_argument('--sanitize',action='store_true')
    args=parser.parse_args()
    host=args.host_source.read_text(); guest=args.guest_source.read_text()
    process=(args.guest_source.parent/'process_bionic.cc').read_text()
    modern='int gesher_fclose_state(' in host
    pieces={
        '@PARSE@':section(host,'bool ParseMode(','// Random characters'),
        '@HOST_CLOSE@':section(host,'int gesher_fclose_state(' if modern else 'int fclose(FILE* stream)','// The stream is reopened'),
        '@HOST_REOPEN@':section(host,'FILE* gesher_freopen_state(' if modern else 'FILE* freopen(','char* mkdtemp('),
        '@STATE@':section(guest,'constexpr uint32_t kWeof','class StreamLock'),
        '@MODE@':section(guest,'std::string CrtMode(','}  // namespace\n\nint CloseBionicStream'),
        '@CLOSE@':section(guest,'int CloseBionicStream(','namespace {\n\n// /dev/zero'),
        '@REOPEN@':section(guest,'FILE* BIONIC_SYSV Freopen(','int BIONIC_SYSV Fgetpos('),
        '@PCLOSE@':section(process,'int BIONIC_SYSV Pclose(','// --- posix_spawn'),
    }
    template=(ROOT/'tests/native/stdio_lifetime_host_test.cc.in').read_text()
    for marker,value in pieces.items():
        print(marker,hashlib.sha256(value.encode()).hexdigest(),flush=True)
        if marker=='@HOST_REOPEN@': value=value.replace('return freopen(path, mode, stream);','return host::freopen(path, mode, stream);')
        if marker=='@CLOSE@':
            value=value.replace('gesher_fclose_state(', 'host::gesher_fclose_state(').replace('std::fclose(', 'host::fclose(')
        if marker=='@REOPEN@':
            value=value.replace('gesher_freopen_state(', 'host::gesher_freopen_state(').replace('std::freopen(', 'host::freopen(')
        assert template.count(marker)==1,marker
        template=template.replace(marker,value)
    with tempfile.TemporaryDirectory(prefix='gesher-stdio-lifetime-') as folder:
        path=pathlib.Path(folder); source=path/'host.cc';binary=path/'test';source.write_text(template)
        command=shlex.split(args.cxx)+['-std=c++20','-O1','-g','-Wall','-Wextra','-Werror','-fno-builtin',str(source),'-o',str(binary)]
        if args.sanitize:command+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
        subprocess.run(command,check=True,timeout=120)
        env=dict(os.environ)
        if args.sanitize:env['UBSAN_OPTIONS']='halt_on_error=1'
        return subprocess.run([str(binary)],cwd=folder,env=env,timeout=30).returncode
if __name__=='__main__':raise SystemExit(main())
