import copy
from pathlib import Path
import types
import unittest
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

if __name__=='__main__':unittest.main()
