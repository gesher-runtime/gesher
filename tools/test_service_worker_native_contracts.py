#!/usr/bin/env python3
"""Execute native worker-unavailable gates and independent storage controller routing.

Prepared worker admission/completion/expiry is separately exercised as isolated
internal code, not active production support. All COM/task boundaries are recorded.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
from test_webview_owned_streams import function
ROOT=Path(__file__).resolve().parents[1]

def compile_and_run(fixture, args, label, expected_failures=(), require_filter_install=False):
    with tempfile.TemporaryDirectory(prefix='gesher-worker-native-') as tmp:
        cpp=Path(tmp)/'contracts.cc';exe=Path(tmp)/'contracts';cpp.write_text(fixture)
        cmd=shlex.split(args.cxx)+['-std=c++20','-Wall','-Wextra','-Werror','-fno-exceptions','-g','-O1',str(cpp),'-o',str(exe)]
        if args.sanitize:cmd+=['-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer']
        subprocess.run(cmd,check=True,timeout=90)
        result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=15)
        if expected_failures:
            if result.returncode!=1 or any(message not in result.stderr for message in expected_failures):
                raise RuntimeError(label+' negative control did not fail as expected: '+result.stdout+result.stderr)
            detail=''
            if require_filter_install:
                match=re.search(r'Recorded worker-filter installations: (\d+)',result.stdout)
                if not match or int(match.group(1))==0:
                    raise RuntimeError('Historical activation did not actually reach filter installation: '+result.stdout+result.stderr)
                detail=' ('+match.group(1)+' recorded worker-filter installations)'
            print(label+' negative control rejected'+detail)
        else:
            print(result.stdout,end='');print(result.stderr,end='');result.check_returncode()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sanitize',action='store_true')
    p.add_argument('--negative-control',action='store_true',
                   help='Require rejection of historical native activation and missing prototype denial')
    p.add_argument('--cxx',default=os.environ.get('CXX','c++'))
    a=p.parse_args()
    source=(ROOT/'compat/webview/webview_host.cc').read_text()
    com=(ROOT/'compat/webview/com_util.h').read_text()
    com=com[com.index('template <typename T>\nclass Com'):com.index('// A string WebView2 allocated')]
    enabled=[line for line in source.splitlines() if line.startswith('bool g_workers_enabled = ')]
    if len(enabled)!=1:raise ValueError('Worker initialization boundary changed')
    enable=function(source,'bool Host::EnableServiceWorkers(')
    if a.negative_control:
        # This is the actual formerly published scheduling/COM/filter flow, not
        # a synthetic false->true result. It safely runs only at recording seams.
        enable=(ROOT/'tests/webview/service_worker_enable_before_gate.cc.in').read_text()
    gate=(ROOT/'tests/webview/service_worker_gate_host.cc.in').read_text()
    for k,v in {'@COM@':com, '@WORKER_ENABLED@':enabled[0], '@ENABLE@':enable,
                '@WITH_CORE@':function(source,'void WithCoreWebView('),
                '@JSON_STRING@':function(source,'std::wstring JsonString('),
                '@QUERY_STORAGE@':function(source,'bool Host::QueryOriginStorage(')}.items():gate=gate.replace(k,v)
    compile_and_run(gate,a,'Historical native worker activation',
                    ('native worker gate schedules no global work',
                     'native worker gate installs no worker filter') if a.negative_control else (),
                    require_filter_install=a.negative_control)
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
    compile_and_run(fixture,a,'Prepared worker missing-denial',
                    ('denial installed before invoking Java',) if a.negative_control else ())
if __name__=='__main__':main()
