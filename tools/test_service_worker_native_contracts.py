#!/usr/bin/env python3
"""Execute native worker admission/completion/expiry at recording COM/task boundaries."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
from test_webview_owned_streams import function
ROOT=Path(__file__).resolve().parents[1]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sanitize',action='store_true')
    p.add_argument('--negative-control',action='store_true')
    a=p.parse_args()
    source=(ROOT/'compat/webview/webview_host.cc').read_text()
    com=(ROOT/'compat/webview/com_util.h').read_text()
    com=com[com.index('template <typename T>\nclass Com'):com.index('// A string WebView2 allocated')]
    state=source[source.index('struct WorkerRequest {'):source.index('\nstd::wstring Headers(ICoreWebView2HttpRequestHeaders* headers);')]
    dispatch=function(source,'bool DispatchServiceWorker(')
    if a.negative_control:
        needle='if (FAILED(args->put_Response(denied))) {\n    ScrubBridgeRequest(request);\n    return false;\n  }'
        assert dispatch.count(needle)==1
        dispatch=dispatch.replace(needle,'(void)denied;')
    fixture=(ROOT/'tests/webview/service_worker_host.cc.in').read_text()
    for k,v in {'@COM@':com,'@STATE@':state,'@DISPATCH@':dispatch,
                '@EXPIRE@':function(source,'void ExpireServiceWorkerRequests('),
                '@CANCEL@':function(source,'void CancelServiceWorkerRequests('),
                '@RESPOND@':function(source,'bool Host::RespondToServiceWorker(')}.items():fixture=fixture.replace(k,v)
    with tempfile.TemporaryDirectory(prefix='gesher-worker-native-') as tmp:
        cpp=Path(tmp)/'contracts.cc';exe=Path(tmp)/'contracts';cpp.write_text(fixture)
        cmd=shlex.split(os.environ.get('CXX','c++'))+['-std=c++20','-Wall','-Wextra','-Werror','-fno-exceptions','-g','-O1',str(cpp),'-o',str(exe)]
        if a.sanitize:cmd+=['-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer']
        subprocess.run(cmd,check=True,timeout=90)
        result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=15)
        if a.negative_control:
            if result.returncode!=1 or 'denial installed before invoking Java' not in result.stderr:
                raise RuntimeError('missing-denial negative control did not fail as expected: '+result.stdout+result.stderr)
            print('Service-worker native negative control: missing denial rejected')
        else:
            print(result.stdout,end='');print(result.stderr,end='');result.check_returncode()
if __name__=='__main__':main()
