#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Fresh-JVM ABBA comparisons of complete pinned-provider HTTP/2 streams.

Both variants compile the full HTTP module and their own provider patches. The
same wire fixture verifies every request header and response header/body byte.
Caller counters include the fixture's checks; process CPU also includes the peer,
provider readers and JIT. They are component measurements, not application/ART
results. Warm cases include exactly64 unmeasured completed streams first.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import statistics
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[1]


def sources(ref):
    if ref:
        paths=subprocess.check_output(['git','ls-tree','-r','--name-only',ref,'--',
                                     'app/java/android/net/http'],cwd=ROOT,text=True).splitlines()
        return {path:hashlib.sha256(subprocess.check_output(['git','show',ref+':'+path],cwd=ROOT)).hexdigest()
                for path in paths if path.endswith('.java')}
    return {path.relative_to(ROOT).as_posix():hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted((ROOT/'app/java/android/net/http').glob('*.java'))}


def percentile(values,p):
    values=sorted(values); rank=(len(values)-1)*p
    lo=math.floor(rank); hi=math.ceil(rank)
    return values[lo]+(values[hi]-values[lo])*(rank-lo)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home',type=Path,required=True)
    parser.add_argument('--okhttp-source',type=Path,required=True)
    parser.add_argument('--baseline-ref',required=True)
    parser.add_argument('--rounds',type=int,default=6)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    if args.rounds<1: parser.error('--rounds must be positive')
    baseline=subprocess.check_output(['git','rev-parse',args.baseline_ref],cwd=ROOT,text=True).strip()
    cases=[('cold_minimal',1,0,0,0),('cold_typical',1,16,4093,0),
           ('warm_minimal',512,0,0,64),('warm_typical',512,16,4093,64),
           ('warm_headers',512,64,4093,64),('warm_many_headers',512,256,0,64)]
    result={'baseline':baseline,'rounds':args.rounds,'ordering':'ABBA in each case/round',
            'percentile':'linear interpolation at (n-1)*p',
            'source_sha256':{'baseline':sources(baseline),'candidate':sources(None)},
            'fixture_sha256':{name:hashlib.sha256((ROOT/name).read_bytes()).hexdigest()
                for name in ['tests/net/Http2BindingsTest.java','tests/net/Http2BindingsLoaderHost.java']},
            'build_output':{},'contract_output':{},'samples':[]}
    java=args.java_home/'bin/java'
    with tempfile.TemporaryDirectory(prefix='gesher-http2-bindings-abba-') as temporary:
        directories={variant:Path(temporary)/variant for variant in ['baseline','candidate']}
        for variant in directories:
            command=[sys.executable,ROOT/'tools/test_http_engine.py','--java-home',args.java_home,
                     '--okhttp-source',args.okhttp_source,'--compile-output',directories[variant]]
            if variant=='baseline': command+=['--source-ref',baseline]
            built=subprocess.run(list(map(str,command)),cwd=ROOT,capture_output=True,text=True,timeout=180,check=True)
            result['build_output'][variant]=built.stdout+built.stderr
            for fixture in ['Http2BindingsTest','Http2BindingsLoaderHost']:
                tested=subprocess.run([str(java),'-cp',str(directories[variant]),fixture],
                                      capture_output=True,text=True,timeout=60,check=True)
                result['contract_output'][variant+'/'+fixture]=tested.stdout+tested.stderr
        for round_number in range(args.rounds):
            for case,count,headers,body,warm in cases:
                for position,variant in enumerate(['baseline','candidate','candidate','baseline']):
                    sample=subprocess.run([str(java),'-cp',str(directories[variant]),'Http2BindingsTest',
                        '--workload',str(count),str(headers),str(body),str(warm)],
                        capture_output=True,text=True,timeout=60,check=True)
                    receipts=[line for line in sample.stdout.splitlines() if line.startswith('HTTP2_BINDINGS_WORK ')]
                    if len(receipts)!=1 or 'Http2BindingsTest passed:' not in sample.stdout:
                        raise RuntimeError('Missing complete contract/measurement receipt: '+sample.stdout+sample.stderr)
                    row={key:int(value) for key,value in (part.split('=') for part in receipts[0].split()[1:])}
                    if row['completeBytes']!=count*body or row['connections']!=1:
                        raise RuntimeError('Incomplete response or connection ownership changed')
                    row.update(case=case,round=round_number,position=position,variant=variant,
                               receipt=sample.stdout,stderr=sample.stderr)
                    result['samples'].append(row)
                print('round='+str(round_number)+' case='+case+' complete',flush=True)
        metrics=['callerCpuNs','callerAllocatedBytes','processCpuNs','elapsedNs']
        result['summary']={}
        for case,*_ in cases:
            result['summary'][case]={}
            for variant in directories:
                rows=[x for x in result['samples'] if x['case']==case and x['variant']==variant]
                result['summary'][case][variant]={key:{'median':statistics.median([x[key] for x in rows]),
                    'p95':percentile([x[key] for x in rows],0.95),
                    'min':min(x[key] for x in rows),'max':max(x[key] for x in rows)} for key in metrics}
        if result['source_sha256']['candidate']!=sources(None):
            raise RuntimeError('Candidate source changed during measurement')
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result['summary'],indent=2),flush=True)


if __name__=='__main__': main()
