#!/usr/bin/env python3
"""Test whole production Bluetooth sockets with controlled native-call boundaries."""
from __future__ import annotations
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BOUNDARIES = {
 'android/content/Context.java': 'package android.content; public class Context {}',
 'android/bluetooth/BluetoothDevice.java': '''package android.bluetooth; public final class BluetoothDevice {
 public static final int ADDRESS_TYPE_PUBLIC=0; private final String address;
 public BluetoothDevice(String address){this.address=address;} public String getAddress(){return address;}
 public long value(){return 1;} public android.content.Context context(){return null;}}
''',
 'android/bluetooth/BluetoothHost.java': '''package android.bluetooth; public final class BluetoothHost {
 public static volatile int target=10000;
 public static android.content.Context context(android.content.Context c){return c;}
 public static int targetSdk(android.content.Context c){return target;}
 public static void enforceConnect(android.content.Context c){} public static boolean on(){return true;}
 public static BluetoothDevice device(String address,int type,android.content.Context context){return new BluetoothDevice(address);}}
''',
 'android/util/Log.java': 'package android.util; public final class Log {public static int w(String tag,String text){return 0;}}',
}


def main() -> None:
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home',type=Path,required=True)
    parser.add_argument('--source-ref')
    parser.add_argument('--case',choices=('all','parallel-connect','closed-accept','peer-eof','single-byte'),default='all')
    args=parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='gesher-rfcomm-') as tmp:
        work=Path(tmp);sources=[]
        for name,content in BOUNDARIES.items():
            path=work/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(content);sources.append(str(path))
        for name in ('BluetoothSocket','BluetoothServerSocket'):
            relative='app/java/android/bluetooth/'+name+'.java';path=ROOT/relative
            if args.source_ref:
                path=work/(name+'.java');path.write_bytes(subprocess.check_output(['git','show',args.source_ref+':'+relative],cwd=ROOT))
            sources.append(str(path))
        sources += [str(p) for p in sorted((ROOT/'tests/bluetooth/socket-host').glob('*.java'))]
        classes=work/'classes';classes.mkdir()
        subprocess.run([str(args.java_home/'bin/javac'),'-encoding','UTF-8','-d',str(classes),*sources],check=True,timeout=60)
        cases=('parallel-connect','closed-accept','peer-eof','single-byte') if args.case=='all' else (args.case,)
        for case in cases:
            subprocess.run([str(args.java_home/'bin/java'),'-cp',str(classes),
                            'android.bluetooth.BluetoothSocketHostTest',case],check=True,timeout=30)


if __name__=='__main__':main()
