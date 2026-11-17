"""Required queue checks must cover every job and reject skipped dependencies."""
import json
from pathlib import Path
import subprocess
import sys
import unittest
import yaml

ROOT = Path(__file__).resolve().parents[2]


class QueueWorkflows(unittest.TestCase):
    def test_both_gates_cover_all_jobs_and_fail_closed(self):
        for workflow, gate in [('ci', 'ci-required'), ('arm64-windows', 'arm64-required')]:
            doc = yaml.safe_load((ROOT / f'.github/workflows/{workflow}.yml').read_text(encoding='utf-8'))
            # PyYAML's YAML 1.1 reader treats the unquoted key 'on' as True.
            self.assertIn('merge_group', doc[True])
            jobs = doc['jobs']
            self.assertEqual(set(jobs[gate]['needs']), set(jobs) - {gate})
            self.assertEqual(jobs[gate]['if'], 'always()')
            body = jobs[gate]['steps'][0]['run'].split("<<'PY'\n", 1)[1].rsplit('\nPY', 1)[0]
            for result in ['success', 'failure', 'cancelled', 'skipped']:
                import os
                env = dict(os.environ, JOB_RESULTS=json.dumps({'producer': {'result': result}}))
                run = subprocess.run([sys.executable, '-c', body], env=env, capture_output=True)
                self.assertEqual(run.returncode == 0, result == 'success')

    def test_no_controller_polling_or_direct_merging(self):
        doc = yaml.safe_load((ROOT / '.github/workflows/automerge.yml').read_text())
        self.assertNotIn('schedule', doc[True])
        self.assertNotIn('workflow_run', doc[True])
        self.assertIn('unlabeled', doc[True]['pull_request_target']['types'])
        script = (ROOT / '.github/scripts/native-merge-queue.js').read_text()
        self.assertNotIn('pulls.merge(', script)
        self.assertIn('expectedHeadOid: $head', script)


if __name__ == '__main__':
    unittest.main()
