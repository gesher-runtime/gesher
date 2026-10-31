import copy
from pathlib import Path
import types
import unittest
g=types.ModuleType('source_guard')
exec(compile(Path(__file__).with_name('source_guard.py').read_bytes(),'source_guard.py','exec'),g.__dict__)

class SourceTests(unittest.TestCase):
    def test_full_inventory_and_negatives(self):
        root=Path(__file__).resolve().parents[2];base=g.inventory(root,g.BASE)
        self.assertEqual(len(base),1776)
        pins={name:'1'*40 for name in g.ADDITIONS}
        current=copy.deepcopy(base);current.update({name:{'mode':'100644','blob':blob}for name,blob in pins.items()})
        g.verify_inventory(base,current,pins)
        for path in base:
            bad=dict(current);bad[path]={'mode':base[path]['mode'],'blob':'0'*40}
            with self.assertRaises(AssertionError):g.verify_inventory(base,bad,pins)
        for path in g.ADDITIONS:
            bad=dict(current);del bad[path]
            with self.assertRaises(AssertionError):g.verify_inventory(base,bad,pins)
        bad=dict(current);bad['unapproved']={'mode':'100644','blob':'1'*40}
        with self.assertRaises(AssertionError):g.verify_inventory(base,bad,pins)

if __name__=='__main__':unittest.main()
