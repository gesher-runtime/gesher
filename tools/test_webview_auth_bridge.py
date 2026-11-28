#!/usr/bin/env python3
"""Run the real WebView authentication coordinator with declared CDP/UI boundaries."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--java-home',required=True,type=Path)
 p.add_argument('--json-classes',required=True,type=Path)
 args=p.parse_args()
 with tempfile.TemporaryDirectory(prefix='gesher-auth-bridge-') as folder:
  out=Path(folder)
  subprocess.run([str(args.java_home/'bin/javac'),'--release','17','-cp',str(args.json_classes),'-d',str(out),
    str(ROOT/'app/java/com/gesher/app/webview/WebViewAuthBridge.java'),str(ROOT/'tests/webview/WebViewAuthBridgeTest.java')],check=True,timeout=120)
  subprocess.run([str(args.java_home/'bin/java'),'-cp',str(out)+os.pathsep+str(args.json_classes),
    'com.gesher.app.webview.WebViewAuthBridgeTest'],check=True,timeout=30)
if __name__=='__main__':main()
