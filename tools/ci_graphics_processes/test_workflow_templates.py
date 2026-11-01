import ast
from pathlib import Path
import types
import unittest
path=Path(__file__).with_name('workflow_templates.py');t=types.ModuleType('workflow_templates')
exec(compile(path.read_bytes(),str(path),'exec'),t.__dict__)

class WorkflowTests(unittest.TestCase):
    def test_templates_and_bootstrap(self):
        c=t.caller_bytes('a'*40).decode();self.assertEqual(c.count('graphicsdex-trusted-source.yml@'+'a'*40),1)
        self.assertIn('needs: [source-authority, fixture-producer]',c)
        self.assertIn('GRAPHICSDEX_MODE: consume',c)
        self.assertIn('cancel-in-progress: false',c)
        self.assertEqual(c.count('scope[\'verify_checkout\']'),2)
        ast.parse(t.BOOT)
        guard=t.guard_bytes({'x':'b'*40},'c'*40).decode()
        self.assertIn("TEMPLATE='"+'c'*40+"'",guard)
        self.assertIn("scope['guard_main'](PINS,TEMPLATE)",guard)
        with self.assertRaises(AssertionError):t.caller_bytes('bad')
        with self.assertRaises(AssertionError):t.guard_bytes({'x':'bad'},'c'*40)

if __name__=='__main__':unittest.main()
