"""Compile and execute selected exact pinned bionic functions in CI only."""
import hashlib
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

PIN = 'ff0e3bf977cd2c7a4ced96b6577ace4b92d82739ba39eaf42cdb4051358eb4f6'


def run_reference(root, output):
    source = root / 'tests/native2/dns-reference/ns_name.c'
    raw = source.read_bytes()
    assert hashlib.sha256(raw).hexdigest() == PIN, 'Pinned AOSP source changed'
    text = raw.decode()
    functions = ['special', 'printable', 'labellen', 'decode_bitstring',
                 'ns_name_ntop', 'ns_name_unpack2', 'ns_name_unpack',
                 'ns_name_uncompress', 'ns_name_skip']
    bodies = []
    for name in functions:
        match = re.search(r'^(?:static )?int\n' + name + r'\(.*?^}', text, re.M | re.S)
        assert match, name
        bodies.append(match.group())
    portability = '''
#include <sys/types.h>
#include <arpa/nameser.h>
#include <resolv.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define NS_TYPE_ELT 0x40
#define DNS_LABELTYPE_BITSTRING 0x41
#define _DIAGASSERT(x) ((void)0)
static const char digits[] = "0123456789";
int ns_name_unpack2(const unsigned char*,const unsigned char*,const unsigned char*,unsigned char*,size_t,size_t*);
'''
    wrappers = '''
int dns_aosp_expand(const unsigned char* m,const unsigned char* e,const unsigned char* s,char* d,int z) {
  if (z < 0) { errno=EMSGSIZE; return -1; }
  int n=ns_name_uncompress(m,e,s,d,(size_t)z);
  if(n>=0 && d[0]=='.')d[0]=0;
  return n;
}
int dns_aosp_skip(const unsigned char* s,const unsigned char* e) {
  const unsigned char* p=s;
  return ns_name_skip(&p,e)<0 ? -1 : (int)(p-s);
}
'''
    driver = Path(shutil.which('g++')).resolve(strict=True)
    c_driver = Path(shutil.which('cc')).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix='dns-aosp-') as temporary:
        path = Path(temporary)
        adapted = path / 'aosp.c'
        adapted.write_text(portability + '\n'.join(bodies) + wrappers)
        executable = path / 'reference'
        renames = ['-D' + name + '=dns_aosp_' + name for name in functions if name.startswith('ns_')]
        defines = [*renames, '-Ddn_expand=dns_aosp_expand', '-Ddn_skipname=dns_aosp_skip']
        c_command = [str(c_driver), '-std=c11', '-D_DEFAULT_SOURCE', '-O2', '-Wall', '-Wextra',
                     '-Werror', *defines, '-c', str(adapted), '-o', str(path / 'aosp.o')]
        subprocess.run(c_command, check=True, timeout=120)
        command = [str(driver), '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
                   '-Wno-deprecated-declarations', *defines, str(path / 'aosp.o'),
                   str(root / 'tests/native2/net_dns_wire.cpp'), '-lresolv', '-o', str(executable)]
        subprocess.run(command, check=True, timeout=120)
        actual = subprocess.run([str(executable)], capture_output=True, text=True, timeout=60)
        print(actual.stdout, end='', flush=True)
        print(actual.stderr, end='', flush=True)
        assert actual.returncode == 0, 'Actual exact pinned AOSP oracle failed'
        (output / 'aosp-reference.log').write_text(actual.stdout)
        extended = subprocess.run([str(executable), '--extended'], capture_output=True, text=True, timeout=60)
        print(extended.stdout, end='', flush=True)
        print(extended.stderr, end='', flush=True)
        assert extended.returncode == 0, 'Actual pinned AOSP extended-label oracle failed'
        (output / 'aosp-extended.log').write_text(extended.stdout)
        return {'role': 'exact-pinned-aosp-name-decoder', 'upstream_sha256': PIN,
                'adapted_sha256': hashlib.sha256(adapted.read_bytes()).hexdigest(),
                'command': command, 'driver_sha256': hashlib.sha256(driver.read_bytes()).hexdigest(),
                'c_command': c_command, 'c_driver_sha256': hashlib.sha256(c_driver.read_bytes()).hexdigest(),
                'executed_binary_sha256': hashlib.sha256(executable.read_bytes()).hexdigest()}
