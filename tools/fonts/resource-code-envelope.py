"""Version1 outer envelope; unchanged three-tool inventory has its OWN EOF.

One existing imports PID receives a bounded legacy substream followed by one
separately typed provider frame. No file opens, discovery or parser subprocess.
"""
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import sys

def load(name):
    spec=importlib.util.spec_from_file_location(name,Path(__file__).with_name(name+'.py'))
    module=importlib.util.module_from_spec(spec);sys.modules[name]=module;spec.loader.exec_module(module)
    return module

def line(stream):
    raw=stream.readline(16385)
    if len(raw)>16384 or not raw.endswith(b'\n'): raise ValueError('bounded outer header')
    def unique(pairs):
        result={}
        for key,value in pairs:
            if key in result: raise ValueError('duplicate outer/provider field')
            result[key]=value
        return result
    return json.loads(raw,object_pairs_hook=unique)

def envelope(stream, legacy, pe, provider):
    request=line(stream)
    provider.require(type(request) is dict and set(request)=={'version','legacy_bytes','context'},'outer schema')
    provider.require(request['version']==1 and type(request['version']) is int,'outer version')
    length=request['legacy_bytes']
    provider.require(type(length) is int and 1<=length<=3*pe.MAX_BYTES+16384+66,'legacy bound')
    data=stream.read(length);provider.require(len(data)==length,'truncated old substream')
    # Materialize before yielding so its unchanged trailing-byte/EOF gate runs.
    records=list(legacy.inventory(io.BytesIO(data),pe))
    for record in records:yield record
    entry=line(stream)
    provider.require(type(entry) is dict and type(entry.get('bytes')) is int and
                     64<=entry['bytes']<=provider.MAX_BYTES,'provider frame bound')
    data=stream.read(entry['bytes']);provider.require(len(data)==entry['bytes'],'provider truncated')
    provider.require(stream.read(len(b'\nFONT_UCRT_CODE_END_V1\n\0\n'))==b'\nFONT_UCRT_CODE_END_V1\n\0\n','provider frame sentinel')
    provider.require(stream.read(1)==b'','outer trailing frame')
    yield provider.admit(data,entry,request['context'])

def main():
    assert os.environ.get('GITHUB_ACTIONS')=='true' and os.name=='nt','Windows CI only'
    for record in envelope(sys.stdin.buffer,load('resource-import-inventory'),
                           load('resource-pe-imports'),load('resource-code-provider')):
        print(json.dumps(record,sort_keys=True),flush=True)

if __name__=='__main__':main()
