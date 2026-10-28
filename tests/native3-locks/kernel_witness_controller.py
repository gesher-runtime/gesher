"""CI only: independent holder, bounded observer, exact owned trace cleanup."""
import argparse
from pathlib import Path
import os
import subprocess
import tempfile

from blocking_oracle import Worker


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--worker', type=Path, required=True)
    args = parser.parse_args()
    worker = args.worker.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix='gesher-kernel-witness-') as directory:
        directory = Path(directory)
        anchor = directory / 'anchor'
        anchor.write_bytes(b'x')
        holder = Worker(worker, 'holder', anchor, 7, 'hold')
        process = None
        passed = False
        try:
            assert holder.receive('held') == [str(holder.process.pid)]
            process = subprocess.Popen([str(worker), 'kernel-witness', str(anchor), '7', 'observe'],
                                       cwd=directory, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                       text=True)
            stdout, stderr = process.communicate(timeout=25)
            print(stdout, end='', flush=True)
            print(stderr, end='', flush=True)
            assert process.returncode == 0, process.returncode
            assert stdout.splitlines().count('KERNEL_WITNESS_SUMMARY actual_internal=1 controls=7 failures=0 public_blocking_closed=false') == 1
            assert any(line.startswith('KERNEL_WAIT ') for line in stdout.splitlines())
            assert not (directory / 'kernel-witness.etl').exists(), 'normal path must delete owned trace'
            passed = True
        finally:
            if process is not None:
                if process.poll() is None:
                    process.kill()
                    process.communicate(timeout=5)
                # Exact process-owned logger only. A failed native assertion must
                # not leave a system-wide trace running after the process dies.
                if not passed:
                    logman = Path(os.environ['SystemRoot']) / 'System32' / 'logman.exe'
                    stopped = subprocess.run([str(logman), 'stop',
                                              f'Gesher.Native3.KernelWitness.{process.pid}', '-ets'],
                                             capture_output=True, text=True, timeout=10)
                    print('KERNEL_FAILURE_CLEANUP', stopped.returncode, stopped.stdout, stopped.stderr, flush=True)
            if holder.process.poll() is None:
                holder.finish('exit')
            else:
                assert holder.process.returncode == 0
        print('KERNEL_CONTROLLER actual_holder=1 actual_internal=1 cleanup=complete public_blocking_closed=false', flush=True)


if __name__ == '__main__':
    main()
