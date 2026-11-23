#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Prove that the real-provider admission tests reject four ownership defects.

Only temporary production copies are mutated. The ordinary fixture and pinned
provider are unchanged, and a failure must name the expected assertion; a
compiler error, unrelated exception or process timeout never counts as a pass.
"""
import argparse
import hashlib
import json
import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUNNER = '''public final class HttpAdmissionNegativeRunner {
 public static void main(String[] args) throws Exception {
  System.setProperty("java.protocol.handler.pkgs","gesher.http.policy");
  Class<?> fixture=Class.forName("HttpEngineAdmissionTest");
  boolean late=args[0].equals("delayedExecutorRejection");
  java.lang.reflect.Method method=late?fixture.getDeclaredMethod(args[0],boolean.class,boolean.class)
      :fixture.getDeclaredMethod(args[0]);
  method.setAccessible(true);
  try { if(late)method.invoke(null,true,true); else method.invoke(null); }
  catch(java.lang.reflect.InvocationTargetException error){error.getCause().printStackTrace();System.exit(1);}
 }
}'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home', type=Path, required=True)
    parser.add_argument('--okhttp-source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    scheduler = 'app/java/android/net/http/HttpRequestScheduler.java'
    engine = 'app/java/android/net/http/JavaHttpEngine.java'
    mutants = [
        ('idle-notification-growth', scheduler,
         'if (tasks.size() > idle && workers < limit)', 'if (idle == 0 && workers < limit)',
         'boundedWorkers', 'An idle worker notification stranded independent queued work'),
        ('completion-blocks-cancellation', scheduler,
         'static final Workers COMPLETION = new Workers("Gesher-HttpEngine-complete", 4,\n'
         '            PROCESS_OUTSTANDING, TimeUnit.SECONDS.toNanos(60));',
         'static final Workers COMPLETION = CLEANUP;',
         'directCallbacksCannotBlockCancellation', 'Application callbacks blocked transport retirement'),
        ('cancellation-releases-live-backend', engine,
         'admission.cancelQueued();', 'admission.cancelQueued(); admission.retire();',
         'uninterruptibleRetirement', 'Cancel released an in-flight backend slot'),
        ('delayed-executor-drops-ownership', engine,
         'discarded(next,e);\n                for (Runnable task; (task=tasks.poll())!=null;) discarded(task,e);',
         'tasks.clear();', 'delayedExecutorRejection', 'Admission owners did not retire'),
    ]
    report = {'source_sha256': {p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest()
                               for p in [scheduler, engine]}, 'negative_controls': []}
    with tempfile.TemporaryDirectory(prefix='gesher-http-negative-') as directory:
        work = Path(directory); classes = work / 'classes'
        subprocess.run(['python3', str(ROOT / 'tools/test_http_engine_policy.py'),
                        '--java-home', str(args.java_home), '--okhttp-source', str(args.okhttp_source),
                        '--compile-output', str(classes)], cwd=ROOT, check=True, timeout=90)
        runner = work / 'HttpAdmissionNegativeRunner.java'; runner.write_text(RUNNER)
        subprocess.run([str(args.java_home / 'bin/javac'), '--release', '17', '-d', str(classes),
                        str(runner)], check=True, timeout=30)
        for name, path, before, after, method, expected in mutants:
            source = (ROOT / path).read_text()
            if source.count(before) != 1:
                raise RuntimeError(f'{name}: source transition is not uniquely identified')
            out = work / name; out.mkdir()
            copied = out / Path(path).name; copied.write_text(source.replace(before, after))
            subprocess.run([str(args.java_home / 'bin/javac'), '--release', '17', '-cp', str(classes),
                            '-d', str(out), str(copied)], check=True, timeout=30)
            result = subprocess.run([str(args.java_home / 'bin/java'), '-cp', os.pathsep.join(map(str, [out, classes])),
                                     'HttpAdmissionNegativeRunner', method], text=True, capture_output=True, timeout=45)
            if result.returncode == 0 or f'java.lang.AssertionError: {expected}' not in result.stderr:
                raise RuntimeError(f'{name}: did not reject the intended defect\n{result.stdout}\n{result.stderr}')
            receipt = {'name': name, 'method': method, 'expected_assertion': expected,
                       'returncode': result.returncode, 'stderr': result.stderr}
            report['negative_controls'].append(receipt)
            print(f'PASS negative control: {name}: {expected}', flush=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
