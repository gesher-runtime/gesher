"""Independently fixed Slice A invariants. Never runs native code."""
import json
CASES={'FS-ALIAS-RW':False,'FS-ALIAS-RO':False,'FS-ALIAS-OFFSET':True}
ONES={'setup','fd_reused','equal','distinct_full_pattern','forward','reverse','source_alive'}
ZEROS={'errno','release_alias','release_source','release_peer','close_original','close_reused','close_replacement','remove_original','remove_other'}
KEYS={'schema','case','page','offset'}|ONES|ZEROS

def verify(text):
    rows=[json.loads(line) for line in text.splitlines() if line.strip()]
    assert len(rows)==len(CASES) and {r['case']for r in rows}==set(CASES)
    for row in rows:
        assert set(row)==KEYS and row['schema']==1
        assert all(type(row[k])is int for k in KEYS-{'case'})
        assert row['page']>=4096 and row['page']&(row['page']-1)==0
        assert row['offset']==(row['page']if CASES[row['case']]else 0)
        assert all(row[k]==1 for k in ONES)
        assert all(row[k]==0 for k in ZEROS)
    return rows
