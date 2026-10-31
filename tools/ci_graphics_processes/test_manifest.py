import copy
import unittest
import types
import tempfile
import io
import zipfile
from pathlib import Path
m=types.ModuleType('manifest')
exec(compile(Path(__file__).with_name('manifest.py').read_bytes(),'manifest.py','exec'),m.__dict__)

class ManifestTests(unittest.TestCase):
    def test_runner_preserves_original_bootstrap(self):
        root=Path(__file__).resolve().parents[2]
        old=(root/'.github/workflows/ci-processes.yml').read_text(encoding='utf-8').split('        run: |\n',1)[1]
        original='\n'.join(line[10:] if line else '' for line in old.splitlines())+'\n'
        runner=(root/'tools/ci_graphics_processes/run-fixture.ps1').read_text(encoding='utf-8')
        start=runner.index('# Additional original dependencies required by the GraphicsDex body.\n')
        end=runner.index('. "$pwd/tests/windows/tracks/84-ci-processes.ps1"',start)
        extra=runner[start:end]
        self.assertIn('tests/windows/helpers/ProcessReaderHandles.ps1',extra)
        self.assertIn('tests/windows/run-tests.ps1',extra)
        self.assertIn("$noDrive.Count -ne 1",extra)
        self.assertEqual(runner[:start]+runner[end:],original)

    def test_binding_and_negative_substitutions(self):
        context={'repository':'gesher-runtime/gesher','repository_id':'1426153186',
                 'source':'a'*40,'run':'123','attempt':'1','guard':'b'*40}
        rows={name:{'mode':'100644','blob':'c'*40,'bytes':1,'sha256':'d'*64} for name in m.PATHS}
        value=m.manifest(context,42,rows)
        self.assertEqual(len(m.validate(value,context,42,rows)),64)
        changes=[lambda x:x['context'].update(source='e'*40),
                 lambda x:x['context'].update(attempt='2'),lambda x:x['context'].update(guard='f'*40),
                 lambda x:x.update(producer_id=43),lambda x:x.update(qualification=0),
                 lambda x:x['members'].pop(m.PATHS[0]),lambda x:x.update(unexpected=True)]
        for change in changes:
            mutated=copy.deepcopy(value);change(mutated)
            with self.assertRaises(AssertionError):m.validate(mutated,context,42,rows)
        def archive(data,extra=False):
            stream=io.BytesIO()
            with zipfile.ZipFile(stream,'w')as z:
                z.writestr('graphicsdex-source-manifest.json',data)
                if extra:z.writestr('extra','synthetic')
            return stream.getvalue()
        self.assertEqual(m.validate_archive(archive(m.canonical(value)+b'\n'),context,42,rows),m.validate(value,context,42,rows))
        with self.assertRaises(AssertionError):m.validate_archive(archive(m.canonical(value)+b'\n',True),context,42,rows)
        with self.assertRaises(AssertionError):m.validate_archive(archive(b'{"schema":1,"schema":2}\n'),context,42,rows)

if __name__=='__main__':unittest.main()
