"""GraphicsDex selected source manifest; external caller must authenticate context.

This module neither authenticates GitHub roles nor runs fixture/native code.
"""
import hashlib
import json
import re
import subprocess
from pathlib import Path

BASE='b0de972bb543e0dd20d125c990817fb5b3407112'
PATHS=('tests/windows/run-tests.ps1','tests/windows/fixtures/Graphics84.cs','tests/windows/fixtures/Gtest84.cs',
       'tests/windows/fixtures/Process84.cs','tests/windows/helpers/CloseProcessReaders.ps1',
       'tests/windows/helpers/GtestProcess.ps1','tests/windows/helpers/ProcessReaderHandles.ps1',
       'tests/windows/tracks/20-libs.ps1','tests/windows/tracks/21-posix.ps1',
       'tests/windows/tracks/30-graphics.ps1','tests/windows/tracks/44-native2.ps1',
       'tests/windows/tracks/49-locale.ps1','tests/windows/tracks/50-services.ps1',
       'tests/windows/tracks/52-surfaces.ps1','tests/windows/tracks/76-media4-opensl.ps1',
       'tests/windows/tracks/84-ci-processes.ps1')

def canonical(value):
    return json.dumps(value,sort_keys=True,separators=(',',':'),ensure_ascii=True).encode()

def selected(root,source):
    root=Path(root)
    def git(*args):return subprocess.check_output(['git','-C',str(root),*args])
    assert git('rev-parse','HEAD').decode().strip()==source
    rows={}
    for name in PATHS:
        baseline=git('rev-parse',BASE+':'+name).decode().strip()
        current=git('rev-parse',source+':'+name).decode().strip()
        assert current==baseline
        raw=git('show',source+':'+name)
        assert (root/name).read_bytes()==raw
        entry=git('ls-tree',source,'--',name).decode().strip().split('\t')
        assert len(entry)==2 and entry[1]==name and entry[0]=='100644 blob '+current
        rows[name]={'mode':'100644','blob':current,'bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest()}
    return rows

def manifest(context,producer_id,rows):
    assert type(context)is dict and set(context)=={'repository','repository_id','source','run','attempt','guard'}
    assert context['repository']=='gesher-runtime/gesher' and context['repository_id']=='1426153186'
    assert all(type(context[k]) is str and re.fullmatch('[0-9a-f]{40}',context[k]) for k in ('source','guard'))
    assert all(type(context[k]) is str and re.fullmatch('[1-9][0-9]{0,19}',context[k]) for k in ('run','attempt'))
    assert type(producer_id)is int and producer_id>0
    assert type(rows)is dict and set(rows)==set(PATHS)
    return {'schema':'GRAPHICSDEX_SOURCE_MANIFEST_V1','context':context,'producer_id':producer_id,'members':rows,'qualification':False}

def validate(value,context,producer_id,rows):
    assert canonical(value)==canonical(manifest(context,producer_id,rows))
    return hashlib.sha256(canonical(value)).hexdigest()
