#!/usr/bin/env python3
"""Run the production Native Wi-Fi adapter against isolated Win32 calls under ASan/UBSan."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home',type=Path,required=True)
    parser.add_argument('--negative-control', choices=('premature-notification', 'skip-eligibility'))
    args=parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='windows-wifi-native-') as temporary:
        output=Path(temporary)/'wifi-native'
        extra=[]
        if args.negative_control:
            source=(ROOT/'compat/net/windows_wifi.cc').read_text()
            before,after = (('state{-2}', 'state{0}') if args.negative_control=='premature-notification'
                            else ('if (!eligible(context))', 'if (false)'))
            if source.count(before)!=1: raise AssertionError('Negative control target changed')
            negative=Path(temporary)/'negative.cc'
            source=source.replace(before,after)
            if args.negative_control=='premature-notification':
                source=source.replace('    wait.items[i].state = 0;\n', '')
            negative.write_text(source)
            extra=['-DGESHER_WIFI_SOURCE="'+str(negative)+'"']
        subprocess.run(['c++','-std=c++17','-g','-O1','-Wall','-Wextra','-Wno-unused-parameter',
                        '-fsanitize=address,undefined','-fno-omit-frame-pointer',
                        '-I'+str(ROOT/'tests/windows-wifi/include'),
                        '-I'+str(args.java_home/'include'),'-I'+str(args.java_home/'include/linux'),
                        *extra,str(ROOT/'tests/windows-wifi/native_host.cc'),'-o',str(output)],check=True)
        result=subprocess.run([str(output)],timeout=20,capture_output=bool(args.negative_control),text=True)
        if args.negative_control:
            expected=('Pre-request notifications' if args.negative_control=='premature-notification'
                      else 'Permission revoked during native initialization')
            if result.returncode==0 or expected not in result.stderr:
                raise AssertionError('Negative control did not fail at the intended production contract: '+result.stderr)
            print('Caught native negative control:',args.negative_control)
        else: result.check_returncode()
if __name__=='__main__':main()
