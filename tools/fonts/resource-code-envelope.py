"""Version3 outer envelope; unchanged three-tool inventory has its OWN EOF.

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

def envelope(stream, legacy, pe, provider, disk, pid):
    request=line(stream)
    provider.require(type(request) is dict and set(request)=={'version','legacy_bytes','context','imports_pid'},'outer schema')
    provider.require(request['version']==3 and type(request['version']) is int,'outer version')
    provider.require(type(pid)is int and 0<pid<=0xffffffff and type(request['imports_pid'])is int and request['imports_pid']==pid,'outer actual imports PID')
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
    provider.require(stream.read(len(b'\nFONT_UCRT_CODE_END_V2\n\0\n'))==b'\nFONT_UCRT_CODE_END_V2\n\0\n','provider frame sentinel')
    yield provider.admit(data,entry,request['context'])
    entry=line(stream)
    provider.require(type(entry)is dict and type(entry.get('bytes'))is int and 64<=entry['bytes']<=disk.DISK_MAX,'disk frame bound')
    data=stream.read(entry['bytes']);provider.require(len(data)==entry['bytes'],'disk frame truncated')
    provider.require(stream.read(len(b'\nFONT_NTDLL_DISK_END_V1\n\0\n'))==b'\nFONT_NTDLL_DISK_END_V1\n\0\n','disk frame sentinel')
    provider.require(stream.read(1)==b'','outer trailing frame')
    yield disk.admit_disk_metadata(data,entry,request['context'],pid)

def output_lines(records):
    lines=[json.dumps(record,sort_keys=True)for record in records]
    # Preserve the original parent's 262144-character pipe cap, including CRLF.
    # Never truncate a language/string category to fit it.
    if sum(len(value)+2 for value in lines)>262144:raise ValueError('bounded complete metadata output')
    return lines

def main():
    assert os.environ.get('GITHUB_ACTIONS')=='true' and os.name=='nt','Windows CI only'
    records=envelope(sys.stdin.buffer,load('resource-import-inventory'),
                     load('resource-pe-imports'),load('resource-code-provider'),load('resource-witness'),os.getpid())
    for encoded in output_lines(records):print(encoded,flush=True)

if __name__=='__main__':main()
