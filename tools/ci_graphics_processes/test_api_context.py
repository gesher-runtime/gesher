import copy
import hashlib
from pathlib import Path
import types
import unittest
from unittest.mock import patch
m=types.ModuleType('api_context')
exec(compile(Path(__file__).with_name('api_context.py').read_bytes(),'api_context.py','exec'),m.__dict__)

class ContextTests(unittest.TestCase):
    def test_strict_context(self):
        e={'GITHUB_ACTIONS':'true','GITHUB_JOB':'fixture-producer','GITHUB_REPOSITORY':'gesher-runtime/gesher','GITHUB_REPOSITORY_ID':'1426153186','GITHUB_REF':'refs/heads/wip/ci-graphics-processes','GITHUB_ACTOR':'palmoni5','GITHUB_TRIGGERING_ACTOR':'palmoni5','GITHUB_EVENT_NAME':'push','GITHUB_SHA':'a'*40,'GITHUB_WORKFLOW_SHA':'a'*40,'GITHUB_RUN_ID':'42','GITHUB_RUN_ATTEMPT':'1'}
        self.assertEqual(m.context(e,'fixture-producer')['source'],'a'*40)
        for key,value in [('GITHUB_ACTIONS','false'),('GITHUB_JOB','other'),('GITHUB_REPOSITORY_ID','1'),('GITHUB_REF','refs/heads/main'),('GITHUB_TRIGGERING_ACTOR','other'),('GITHUB_EVENT_NAME','pull_request'),('GITHUB_WORKFLOW_SHA','b'*40),('GITHUB_RUN_ATTEMPT','01')]:
            bad=copy.deepcopy(e);bad[key]=value
            with self.assertRaises(AssertionError):m.context(bad,'fixture-producer')
        with self.assertRaises(AssertionError):m.api('../outside')
        class Response:
            def __enter__(self):return self
            def __exit__(self,*args):pass
            def read(self,n):return b'SYNTHETIC_ZIP'
        class Opener:
            def open(self,*args,**kwargs):return Response()
        with patch.dict(m.os.environ,{'GITHUB_ACTIONS':'true','GH_TOKEN':'SYNTHETIC'}),patch.object(m.urllib.request,'build_opener',return_value=Opener()):
            self.assertEqual(m.download_artifact(5,hashlib.sha256(b'SYNTHETIC_ZIP').hexdigest()),b'SYNTHETIC_ZIP')
            with self.assertRaises(AssertionError):m.download_artifact(5,'0'*64)

if __name__=='__main__':unittest.main()
