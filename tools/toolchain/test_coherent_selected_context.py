"""Pure identity controls for WIP coherent selected full caller."""
import copy
from pathlib import Path
import re
import coherent_selected_context as c

def main():
 source='1'*40
 caller=(Path(__file__).resolve().parents[2]/c.PATH).read_text(encoding='utf-8')
 pins=re.findall(r'(?m)^    uses: '+re.escape(c.REPOSITORY+'/'+c.POLICY_PATH+'@')+r'([0-9a-f]{40})$',caller)
 assert len(pins)==1;guard=pins[0]
 env=dict(GITHUB_ACTIONS='true',GITHUB_EVENT_NAME='workflow_dispatch',GITHUB_ACTOR='palmoni5',
  GITHUB_TRIGGERING_ACTOR='palmoni5',GITHUB_REPOSITORY=c.REPOSITORY,GITHUB_REPOSITORY_ID=c.REPOSITORY_ID,
  GITHUB_REF=c.TUPLE['ref'],GITHUB_WORKFLOW=c.WORKFLOW,GITHUB_WORKFLOW_REF=c.TUPLE['workflow_ref'],
  GITHUB_JOB='build',GESHER_HOST_PTHREAD_CALLER=c.CALLER,GESHER_HOST_PTHREAD_SELECTION='selected-minimum',
  GITHUB_SHA=source,GITHUB_WORKFLOW_SHA=source,GESHER_COHERENT_FULL_AUTHORIZED_SOURCE=source,
  GITHUB_RUN_ID='123',GITHUB_RUN_ATTEMPT='1',GESHER_COHERENT_FULL_POLICY_REF=c.REPOSITORY+'/'+c.POLICY_PATH+'@'+guard)
 assert c.context(env,'selected-minimum')['GITHUB_SHA']==source
 descriptor=c.descriptor(env);assert descriptor['policy_ref']==env['GESHER_COHERENT_FULL_POLICY_REF']
 c.check_descriptor(descriptor,env)
 for key,value in (('policy_ref',c.REPOSITORY+'/'+c.POLICY_PATH+'@'+'f'*40),('source','f'*40),('caller','foreign')):
  bad=dict(descriptor);bad[key]=value
  try:c.check_descriptor(bad,env)
  except (AssertionError,KeyError):pass
  else:raise AssertionError('invalid descriptor accepted: '+key)
 for key in env:
  bad=copy.copy(env);bad[key]='invalid'
  try:c.context(bad,'selected-minimum')
  except (AssertionError,KeyError):pass
  else:raise AssertionError('invalid caller accepted: '+key)
 for selection in ('publisher-control','',None):
  try:c.context(env,selection)
  except AssertionError:pass
  else:raise AssertionError('invalid selected role accepted')
 print('COHERENT_SELECTED_CONTEXT_PURE_PASS fields='+str(len(env))+' roles=3 actual_authority=false')

if __name__=='__main__':main()
