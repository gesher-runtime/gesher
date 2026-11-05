"""Pure negative controls for same-run selected TCP roles."""
import copy
import coherent_tcp_context as c

def main():
 source='1'*40
 base=dict(GITHUB_ACTIONS='true',GITHUB_REPOSITORY=c.REPOSITORY,GITHUB_REPOSITORY_ID=c.REPOSITORY_ID,
  GITHUB_REF='refs/heads/'+c.BRANCH,GITHUB_WORKFLOW=c.WORKFLOW,GITHUB_WORKFLOW_REF=c.REPOSITORY+'/'+c.PATH+'@refs/heads/'+c.BRANCH,
  GITHUB_EVENT_NAME='workflow_dispatch',GITHUB_ACTOR='palmoni5',GITHUB_TRIGGERING_ACTOR='palmoni5',
  GITHUB_SHA=source,GITHUB_WORKFLOW_SHA=source,GESHER_COHERENT_TCP_AUTHORIZED_SOURCE=source,
  GESHER_COHERENT_TCP_POLICY_REF=c.REPOSITORY+'/'+c.PATH+'@'+source,GITHUB_RUN_ID='123',GITHUB_RUN_ATTEMPT='1')
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
  b=dict(e,GESHER_COHERENT_TCP_POLICY_REF=c.REPOSITORY+'/'+c.PATH+'@'+'f'*40)
  try:c.context(b,job)
  except AssertionError:count+=1
  else:raise AssertionError('policy from a different commit accepted')
 # PR metadata uses the head SHA, while every produced manifest uses the merge
 # checkout. All four selected TCP roles must validate the same PR authority.
 repo=dict(id=int(c.REPOSITORY_ID),full_name=c.REPOSITORY)
 event=dict(number=5,pull_request=dict(merge_commit_sha=source,
  base=dict(ref='main',repo=dict(repo),sha='3'*40),head=dict(ref='codex/test',repo=dict(repo),sha='2'*40)))
 parents=('3'*40,'2'*40)
 pr=dict(base,GITHUB_EVENT_NAME='pull_request',GITHUB_REF='refs/pull/5/merge',
  GITHUB_BASE_REF='main',GITHUB_HEAD_REF='codex/test',
  GITHUB_WORKFLOW_REF=c.REPOSITORY+'/'+c.PATH+'@refs/pull/5/merge')
 for job in ('compile','linux-reference','windows','supplier-correlation'):
  e=dict(pr,GITHUB_JOB=job)
  if job!='linux-reference':e['TCP_SUPPLIER_ROLE']='selected-minimum'
  assert c.context(e,job,event,parents)['GITHUB_SHA']==source
  for k in e:
   b=dict(e);b[k]='invalid'
   try:c.context(b,job,event,parents)
   except (AssertionError,KeyError):count+=1
   else:raise AssertionError('wrong PR role identity accepted '+job+'/'+k)
  foreign=copy.deepcopy(event);foreign['pull_request']['head']['repo']['id']=1
  try:c.context(e,job,foreign,parents)
  except AssertionError:count+=1
  else:raise AssertionError('foreign PR supplier authority accepted')
 print('COHERENT_TCP_CONTEXT_PURE_PASS roles=4 negatives='+str(count)+' actual_authority=false')

if __name__=='__main__':main()
