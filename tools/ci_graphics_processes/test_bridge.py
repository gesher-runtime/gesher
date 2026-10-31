import copy
from pathlib import Path
import types
import unittest
from unittest.mock import patch
path=Path(__file__).with_name('bridge.py');m=types.ModuleType('bridge');m.__file__=str(path)
exec(compile(path.read_bytes(),str(path),'exec'),m.__dict__)

class BridgeTests(unittest.TestCase):
    def test_producer_identity(self):
        c={'source':'a'*40,'run':'12','attempt':'1'}
        row={'name':'fixture-producer','head_sha':c['source'],'run_id':12,'run_attempt':1,'status':'in_progress','conclusion':None,'id':42}
        jobs={'total_count':1,'jobs':[row]};self.assertEqual(m.producer_job(jobs,c)['id'],42)
        for key,value in [('head_sha','b'*40),('run_attempt',2),('status','completed'),('id',True),('name','other')]:
            bad=copy.deepcopy(jobs);bad['jobs'][0][key]=value
            with self.assertRaises(AssertionError):m.producer_job(bad,c)
        dup={'total_count':2,'jobs':[row,row]}
        with self.assertRaises(AssertionError):m.producer_job(dup,c)
        self.assertEqual(m.manifest_context(c,'b'*40),{'repository':'gesher-runtime/gesher','repository_id':'1426153186','source':'a'*40,'run':'12','attempt':'1','guard':'b'*40})
    def test_consumer_binds_current_roles_and_artifact(self):
        c={'source':'a'*40,'run':'12','attempt':'1'}
        def job(name,id,status,conclusion):return {'name':name,'id':id,'head_sha':c['source'],'run_id':12,'run_attempt':1,'status':status,'conclusion':conclusion,'started_at':'2026-10-31T10:00:00Z','completed_at':'2026-10-31T10:01:00Z'}
        jobs={'total_count':3,'jobs':[job('source-authority / verify',1,'completed','success'),job('fixture-producer',2,'completed','success'),job('fixture (windows-2022)',3,'in_progress',None)]}
        artifact={'id':99,'name':'graphicsdex-source-'+c['source']+'-12-1','expired':False,'size_in_bytes':100,'digest':'sha256:'+'d'*64,'created_at':'2026-10-31T10:00:30Z','workflow_run':{'id':12,'head_sha':c['source'],'repository_id':1426153186,'head_repository_id':1426153186}}
        with patch.dict(m.os.environ,{'RUNNER_OS':'Windows','ImageOS':'win22'}):
            self.assertEqual(m.consumer_artifact(c,jobs,{'total_count':1,'artifacts':[artifact]})[1],2)
            for key,value in [('head_sha','c'*40),('run_attempt',2),('status','completed')]:
                bad=copy.deepcopy(jobs);bad['jobs'][2][key]=value
                with self.assertRaises(AssertionError):m.consumer_artifact(c,bad,{'total_count':1,'artifacts':[artifact]})
            bad=copy.deepcopy(jobs);bad['jobs'][1]['conclusion']='failure'
            with self.assertRaises(AssertionError):m.consumer_artifact(c,bad,{'total_count':1,'artifacts':[artifact]})
        jobs25=copy.deepcopy(jobs);jobs25['jobs'][2]['name']='fixture (windows-2025)'
        for img in ('win25','win25-vs2026'):
            with patch.dict(m.os.environ,{'RUNNER_OS':'Windows','ImageOS':img}):
                self.assertEqual(m.consumer_artifact(c,jobs25,{'total_count':1,'artifacts':[artifact]})[1],2)

if __name__=='__main__':unittest.main()
