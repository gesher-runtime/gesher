"""Bounded transport retries for an exact GitHub JSON GET, never its acceptance."""
import argparse
import json
import re
import subprocess
import sys
import time


_BACKOFF_SECONDS = (1, 2, 4)
_HTTP_STATUS = re.compile(r'\(HTTP ([1-5][0-9]{2})\)')
_STREAM_CANCEL = re.compile(
    r'(?:^|: )stream error: stream ID [1-9][0-9]*; CANCEL; received from peer$')
_DIAL_TIMEOUT = re.compile(
    r'(?:gh: )?(?:Get "https://[^"\r\n]+": )?dial tcp '
    r'(?:[0-9]{1,3}(?:\.[0-9]{1,3}){3}|\[[0-9A-Fa-f:.]+\]):443: i/o timeout')


class GitHubReadError(RuntimeError):
    """The exact read failed; callers must not accept or consume partial output."""


def _reject_json_constant(value):
    # Python accepts these JavaScript extensions by default; GitHub's response
    # contract is JSON, so NaN/Infinity are parse failures rather than metadata.
    raise ValueError(f'Non-JSON constant: {value}')


def _transient(stderr):
    # The status reported by gh takes precedence over transport-looking text in
    # an error message. In particular, never turn a 401/403/404 into a retry.
    statuses = _HTTP_STATUS.findall(stderr)
    if statuses:
        return all(status in ('502', '503', '504') for status in statuses)
    # Go's HTTPS dial timeout precedes any response. Require its single
    # diagnostic so it cannot override an additional error from gh.
    return bool(_DIAL_TIMEOUT.fullmatch(stderr.strip())) or any(
        _STREAM_CANCEL.search(line) for line in stderr.splitlines())


def get_json(endpoint):
    """GET endpoint verbatim, retrying only the observed transient failures.

    There are at most four requests, with 1/2/4-second delays. JSON parsing and
    all caller identity/digest predicates are outside the retry decision.
    """
    if not isinstance(endpoint, str) or not endpoint:
        raise ValueError('An exact GitHub endpoint is required')
    # End option parsing: even an unusual endpoint cannot introduce a method,
    # body, host, pagination or other gh option. No shell is involved.
    command = ['gh', 'api', '--method', 'GET', '--', endpoint]
    for attempt in range(len(_BACKOFF_SECONDS) + 1):
        try:
            result = subprocess.run(command, capture_output=True, timeout=60)
        except (OSError, subprocess.TimeoutExpired) as error:
            # Neither local launch failures nor generic timeouts were observed
            # as retryable failures; fail closed without broadening the policy.
            raise GitHubReadError(
                f'GitHub GET could not complete ({type(error).__name__})') from None
        stderr = result.stderr.decode('utf-8', errors='replace')
        if stderr:
            sys.stderr.write(stderr)
            if not stderr.endswith('\n'):
                sys.stderr.write('\n')
        if result.returncode == 0:
            try:
                return json.loads(result.stdout, parse_constant=_reject_json_constant)
            except (ValueError, UnicodeDecodeError) as error:
                raise GitHubReadError('GitHub GET returned invalid JSON') from error
        if attempt == len(_BACKOFF_SECONDS) or not _transient(stderr):
            raise GitHubReadError(
                f'GitHub GET failed after {attempt + 1} attempt(s) '
                f'(gh exit {result.returncode})')
        delay = _BACKOFF_SECONDS[attempt]
        print(f'CI_GITHUB_READ_RETRY attempt={attempt + 1}/4 delay={delay}s',
              file=sys.stderr, flush=True)
        time.sleep(delay)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('endpoint', help='exact gh api endpoint, including any query')
    args = parser.parse_args(argv)
    try:
        value = get_json(args.endpoint)
    except (GitHubReadError, ValueError) as error:
        print(f'CI_GITHUB_READ_ERROR {error}', file=sys.stderr)
        return 1
    # The PowerShell interface consumes only complete JSON on stdout.
    print(json.dumps(value, ensure_ascii=True))
    return 0


if __name__ == '__main__':
    sys.exit(main())
