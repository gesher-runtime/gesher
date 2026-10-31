"""Pure negative controls for same-run selected TCP roles."""
import copy,re
from pathlib import Path
import coherent_tcp_context as c

def main():
 raw=(Path(__file__).resolve().parents[2]/c.PATH).read_text(encoding='utf-8')
 pins=re.findall(r'(?m)^    uses: '+re.escape(c.REPOSITORY+'/'+c.TRUST+'@')+r'([0-9a-f]{40})$',raw);assert len(pins)==1
 source='1'*40
 base=dict(GITHUB_ACTIONS='true',GITHUB_REPOSITORY=c.REPOSITORY,GITHUB_REPOSITORY_ID=c.REPOSITORY_ID,
  GITHUB_REF='refs/heads/'+c.BRANCH,GITHUB_WORKFLOW=c.WORKFLOW,GITHUB_WORKFLOW_REF=c.REPOSITORY+'/'+c.PATH+'@refs/heads/'+c.BRANCH,
  GITHUB_EVENT_NAME='workflow_dispatch',GITHUB_ACTOR='palmoni5',GITHUB_TRIGGERING_ACTOR='palmoni5',
  GITHUB_SHA=source,GITHUB_WORKFLOW_SHA=source,GESHER_COHERENT_TCP_AUTHORIZED_SOURCE=source,
  GESHER_COHERENT_TCP_POLICY_REF=c.REPOSITORY+'/'+c.TRUST+'@'+pins[0],GITHUB_RUN_ID='123',GITHUB_RUN_ATTEMPT='1')
 count=0
 for job in ('compile','linux-reference','windows','supplier-correlation'):
  e=dict(base,GITHUB_JOB=job)
  if job!='linux-reference':e['TCP_SUPPLIER_ROLE']='selected-minimum'
  assert c.context(e,job)['GITHUB_SHA']==source
  for k in e:
   b=copy.copy(e);b[k]='invalid'
   try:c.context(b,job)
   except (AssertionError,KeyError):count+=1
   else:raise AssertionError('wrong role identity accepted '+job+'/'+k)
  b=dict(e,GESHER_COHERENT_TCP_POLICY_REF=c.REPOSITORY+'/'+c.TRUST+'@'+'f'*40)
  try:c.context(b,job)
  except AssertionError:count+=1
  else:raise AssertionError('different valid immutable guard accepted')
 print('COHERENT_TCP_CONTEXT_PURE_PASS roles=4 negatives='+str(count)+' actual_authority=false')

if __name__=='__main__':main()
