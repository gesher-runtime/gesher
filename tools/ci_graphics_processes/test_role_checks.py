import copy
from pathlib import Path
import types
import unittest
r=types.ModuleType('role_checks')
exec(compile(Path(__file__).with_name('role_checks.py').read_bytes(),'role_checks.py','exec'),r.__dict__)

class RoleTests(unittest.TestCase):
    def test_current_run_and_roles(self):
        c={'source':'a'*40,'run':'12','attempt':'1'};g='b'*40
        repo={'id':r.REPOSITORY_ID,'full_name':r.REPOSITORY}
        run={'id':12,'run_attempt':1,'head_sha':c['source'],'name':r.WORKFLOW,'path':r.CALLER,'head_branch':r.BRANCH,'event':'push','actor':{'login':'palmoni5'},'triggering_actor':{'login':'palmoni5'},'repository':repo,'head_repository':repo,'referenced_workflows':[{'path':r.REPOSITORY+'/'+r.GUARD+'@'+g,'sha':g}]}
        r.verify_run(run,c,g)
        for mutate in [lambda x:x.update(run_attempt=2),lambda x:x.update(head_sha='c'*40),lambda x:x['actor'].update(login='other'),lambda x:x['referenced_workflows'][0].update(sha='d'*40)]:
            bad=copy.deepcopy(run);mutate(bad)
            with self.assertRaises(AssertionError):r.verify_run(bad,c,g)
        job={'id':42,'head_sha':c['source'],'run_id':12,'run_attempt':1,'status':'completed','conclusion':'success','name':'fixture-producer'}
        self.assertEqual(r.verify_jobs({'total_count':1,'jobs':[job]},c,['fixture-producer'])['fixture-producer']['id'],42)
        for change in ('run_attempt','head_sha','conclusion'):
            bad=copy.deepcopy(job);bad[change]={'run_attempt':2,'head_sha':'c'*40,'conclusion':'failure'}[change]
            with self.assertRaises(AssertionError):r.verify_jobs({'total_count':1,'jobs':[bad]},c,['fixture-producer'])
        producer={'started_at':'2026-10-31T10:00:00Z','completed_at':'2026-10-31T10:01:00Z'}
        artifact={'id':99,'name':'graphicsdex-source-'+c['source']+'-12-1','expired':False,'size_in_bytes':100,'digest':'sha256:'+'d'*64,'created_at':'2026-10-31T10:00:30Z','workflow_run':{'id':12,'head_sha':c['source'],'repository_id':r.REPOSITORY_ID,'head_repository_id':r.REPOSITORY_ID}}
        self.assertEqual(r.verify_artifact({'total_count':1,'artifacts':[artifact]},c,producer)['id'],99)
        for mutate in [lambda x:x.update(expired=True),lambda x:x.update(digest='sha256:bad'),lambda x:x.update(created_at='2026-10-31T10:02:00Z'),lambda x:x['workflow_run'].update(head_sha='e'*40)]:
            bad=copy.deepcopy(artifact);mutate(bad)
            with self.assertRaises(AssertionError):r.verify_artifact({'total_count':1,'artifacts':[bad]},c,producer)

if __name__=='__main__':unittest.main()
