"""Synthetic metadata negatives only, not runtime evidence."""
import copy,json
from oracle import CASES,ONES,ZEROS,verify
rows=[dict(schema=1,case=name,page=4096,offset=4096 if offset else 0,
           **{k:1 for k in ONES},**{k:0 for k in ZEROS})for name,offset in CASES.items()]
def encode(rows):return '\n'.join(json.dumps(row)for row in rows)
def reject(rows):
    try:verify(encode(rows))
    except (AssertionError,KeyError,TypeError):return
    raise AssertionError('mutant accepted')
assert verify(encode(rows))==rows
count=0
for index in range(3):
    for field in ONES|ZEROS|{'schema','page','offset'}:
        bad=copy.deepcopy(rows);bad[index][field]=-1;reject(bad);count+=1
    bad=copy.deepcopy(rows);bad[index]['extra']=1;reject(bad);count+=1
for bad in (rows[:-1],rows+rows[:1],[rows[0]]*3):reject(bad);count+=1
print('PURE_ORACLE_PASS negatives='+str(count)+' synthetic_only=true native_execution=false')
