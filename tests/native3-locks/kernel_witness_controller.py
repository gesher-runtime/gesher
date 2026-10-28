"""CI only: independent holder, bounded observer, exact owned trace cleanup."""
import argparse
from pathlib import Path
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
        primary_error = None
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
        except BaseException as error:
            primary_error = error
        finally:
            cleanup_errors = []
            if process is not None:
                try:
                    if process.poll() is None:
                        process.kill()
                    stdout, stderr = process.communicate(timeout=5)
                    if not passed:
                        print(stdout, stderr, end='', flush=True)
                except BaseException as error:
                    cleanup_errors.append(('native process retirement', repr(error)))
                # Exact process-owned logger only. A failed native assertion must
                # not leave a system-wide trace running after the process dies.
                if not passed:
                    try:
                        stopped = subprocess.run([str(worker), 'kernel-cleanup', 'unused',
                                                  str(process.pid), 'unused'], cwd=directory,
                                                 capture_output=True, text=True, timeout=10)
                        print(stopped.stdout, stopped.stderr, end='', flush=True)
                        assert stopped.returncode == 0, ('native trace cleanup exit', stopped.returncode)
                        assert stopped.stdout.splitlines() == ['KERNEL_FAILURE_CLEANUP session=absent']
                    except BaseException as error:
                        cleanup_errors.append(('exact trace stop/absence proof', repr(error)))
            try:
                if holder.process.poll() is None:
                    holder.finish('exit')
                else:
                    assert holder.process.returncode == 0
            except BaseException as error:
                cleanup_errors.append(('holder graceful retirement', repr(error)))
                try:
                    if holder.process.poll() is None:
                        holder.process.kill()
                    holder.process.wait(timeout=5)
                    holder.reader.join(timeout=2)
                    assert not holder.reader.is_alive(), 'holder reader failed final retirement'
                    holder.process.stdin.close()
                    holder.process.stdout.close()
                except BaseException as error:
                    cleanup_errors.append(('holder forced retirement', repr(error)))
            if primary_error is not None:
                if cleanup_errors:
                    print('KERNEL_CLEANUP_ERRORS', cleanup_errors, flush=True)
                raise primary_error
            assert not cleanup_errors, cleanup_errors
        print('KERNEL_CONTROLLER actual_holder=1 actual_internal=1 cleanup=complete public_blocking_closed=false', flush=True)


if __name__ == '__main__':
    main()
