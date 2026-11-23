"""Bounded transport retries for an exact GitHub JSON GET, never its acceptance."""
import argparse
from email.utils import parsedate_to_datetime
import json
import math
import re
import subprocess
import sys
import time


_BACKOFF_SECONDS = (1, 2, 4)
_MAX_ELAPSED_SECONDS = 4 * 60 + sum(_BACKOFF_SECONDS)
# The repository's GITHUB_TOKEN quota is shared by every concurrent job and
# resets hourly. Waiting up to 15 minutes for a published reset is cheaper
# than failing the job and rerunning the whole ci run (about 500 more calls).
_MAX_RATE_WAIT_SECONDS = 15 * 60
_RATE_HEADERS = ('date', 'retry-after', 'x-ratelimit-remaining', 'x-ratelimit-reset',
                 'x-ratelimit-limit', 'x-ratelimit-used', 'x-ratelimit-resource')
_HTTP_STATUS = re.compile(r'\(HTTP ([1-5][0-9]{2})\)')
_GH_HTTP_STATUS = re.compile(r'^gh: HTTP ([1-5][0-9]{2})\r?$', re.MULTILINE)
_STREAM_CANCEL = re.compile(
    r'(?:^|: )stream error: stream ID [1-9][0-9]*; CANCEL; received from peer$')
_DIAL_TIMEOUT = re.compile(
    r'(?:gh: )?(?:Get "https://[^"\r\n]+": )?dial tcp '
    r'(?:[0-9]{1,3}(?:\.[0-9]{1,3}){3}|\[[0-9A-Fa-f:.]+\]):443: i/o timeout')


class GitHubReadError(RuntimeError):
    """The exact read failed; callers must not accept or consume partial output."""


class GitHubTransientError(GitHubReadError):
    """An observed transient failure that the caller asked not to retry here."""


def _reject_json_constant(value):
    # Python accepts these JavaScript extensions by default; GitHub's response
    # contract is JSON, so NaN/Infinity are parse failures rather than metadata.
    raise ValueError(f'Non-JSON constant: {value}')


def _number(value):
    return int(value) if isinstance(value, str) and re.fullmatch(r'[0-9]{1,20}', value) else None


def rate_limit_info(status, headers, body, failures=0):
    """Recognize GitHub quota responses, never an arbitrary forbidden request.

    This small policy is also kept in the standalone archive downloader, whose
    trusted-source users must not acquire a new unchecked import dependency.
    """
    if status not in (403, 429) or len(body) > 16384:
        return None
    try:
        message = json.loads(body, parse_constant=_reject_json_constant).get('message')
    except (ValueError, UnicodeDecodeError, AttributeError):
        return None
    if not isinstance(message, str):
        return None
    primary = message.startswith('API rate limit exceeded for ')
    secondary = message.startswith('You have exceeded a secondary rate limit')
    if not primary and not secondary:
        return None
    remaining = _number(headers.get('x-ratelimit-remaining'))
    reset = _number(headers.get('x-ratelimit-reset'))
    retry_after = _number(headers.get('retry-after'))
    resource = headers.get('x-ratelimit-resource', '')
    info = dict(kind='primary' if primary else 'secondary', remaining=remaining,
                reset=reset, retry_after=retry_after, delay=None,
                limit=_number(headers.get('x-ratelimit-limit')),
                used=_number(headers.get('x-ratelimit-used')),
                resource=resource if re.fullmatch(r'[a-z_]{1,64}', resource) else None)
    if any(key in headers and number is None for key, number in (
            ('retry-after', retry_after), ('x-ratelimit-remaining', remaining),
            ('x-ratelimit-reset', reset))):
        return info
    delay = 0
    if primary or remaining == 0:
        if remaining != 0 or reset is None:
            return info
        try:
            date = parsedate_to_datetime(headers.get('date', ''))
            if date.tzinfo is None:
                return info
            # Server time avoids retrying early when the runner clock is ahead.
            delay = max(1, math.ceil(reset - date.timestamp() + 1))
        except (ValueError, TypeError, OverflowError):
            return info
    if retry_after is not None:
        delay = max(delay, retry_after, 1)
    elif secondary and remaining != 0:
        delay = 60 * (2 ** failures)
    info['delay'] = delay
    return info


def _http_response(raw):
    """Separate gh --include's single final response from its JSON body."""
    parts = re.split(rb'\r?\n\r?\n', raw, maxsplit=1)
    if len(parts) != 2 or len(parts[0]) > 65536:
        raise GitHubReadError('GitHub GET returned invalid HTTP framing')
    lines = parts[0].splitlines()
    match = re.fullmatch(rb'HTTP/[12](?:\.[0-9])? ([1-5][0-9]{2})(?: [^\r\n]*)?', lines[0]) if lines else None
    if match is None:
        raise GitHubReadError('GitHub GET returned invalid HTTP status')
    headers = {}
    for line in lines[1:]:
        name, separator, value = line.partition(b':')
        if not separator or not re.fullmatch(rb"[!#$%&'*+.^_`|~0-9A-Za-z-]+", name):
            raise GitHubReadError('GitHub GET returned invalid HTTP headers')
        key = name.decode('ascii').lower()
        if key in _RATE_HEADERS:
            if key in headers:
                raise GitHubReadError('GitHub GET returned duplicate quota headers')
            headers[key] = value.decode('ascii', errors='replace').strip()
    return int(match[1]), headers, parts[1]


def _sleep(delay):
    while delay > 0:
        chunk = min(delay, 60)
        time.sleep(chunk)
        delay -= chunk


def _transient(stderr):
    # The status reported by gh takes precedence over transport-looking text in
    # an error message. In particular, never turn a 401/403/404 into a retry.
    statuses = _HTTP_STATUS.findall(stderr) + _GH_HTTP_STATUS.findall(stderr)
    if statuses:
        return all(status in ('502', '503', '504') for status in statuses)
    # Go's HTTPS dial timeout precedes any response. Require its single
    # diagnostic so it cannot override an additional error from gh.
    return bool(_DIAL_TIMEOUT.fullmatch(stderr.strip())) or any(
        _STREAM_CANCEL.search(line) for line in stderr.splitlines())


def get_json(endpoint, *, retry_transient=True):
    """GET endpoint verbatim, retrying only the observed transient failures.

    There are at most four requests. Quota waits require GitHub's message and
    valid headers and fit a 15-minute cumulative wait budget; time spent in
    those waits does not consume the 247-second transport envelope. JSON parsing and all caller
    identity/digest predicates are outside the retry decision. With
    retry_transient=False an observed transient failure raises
    GitHubTransientError at once, so the caller can choose a cheaper fallback.
    """
    if not isinstance(endpoint, str) or not endpoint:
        raise ValueError('An exact GitHub endpoint is required')
    # End option parsing: even an unusual endpoint cannot introduce a method,
    # body, host, pagination or other gh option. No shell is involved.
    command = ['gh', 'api', '--method', 'GET', '--include', '--', endpoint]
    deadline = time.monotonic() + _MAX_ELAPSED_SECONDS
    rate_wait = 0
    rate_failures = 0
    for attempt in range(len(_BACKOFF_SECONDS) + 1):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise GitHubReadError('GitHub GET exceeded its bounded read budget')
        try:
            result = subprocess.run(command, capture_output=True, timeout=min(60, remaining))
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
        status, headers, body = None, {}, b''
        if result.returncode == 0 or result.stdout.startswith(b'HTTP/'):
            status, headers, body = _http_response(result.stdout)
            reported = _HTTP_STATUS.findall(stderr)
            if reported and any(int(value) != status for value in reported):
                raise GitHubReadError('GitHub GET returned conflicting HTTP status evidence')
        if result.returncode == 0:
            if not 200 <= status < 300:
                raise GitHubReadError('GitHub GET did not return a successful HTTP status')
            try:
                return json.loads(body, parse_constant=_reject_json_constant)
            except (ValueError, UnicodeDecodeError) as error:
                raise GitHubReadError('GitHub GET returned invalid JSON') from error
        quota = rate_limit_info(status, headers, body, rate_failures)
        delay = None
        if quota is not None:
            delay = quota['delay']
            permitted = (delay is not None and rate_wait + delay <= _MAX_RATE_WAIT_SECONDS
                         and attempt < len(_BACKOFF_SECONDS))
            print('CI_GITHUB_RATE_LIMIT ' + ' '.join(
                f'{key}={quota[key]}' for key in ('kind', 'limit', 'used', 'remaining', 'resource', 'reset', 'retry_after'))
                + f' action={"wait" if permitted else "fail"}', file=sys.stderr, flush=True)
            if not permitted:
                raise GitHubReadError('GitHub quota cannot recover within bounded read/wait budget')
            rate_wait += delay
            rate_failures += 1
            # The quota wait is not transport time: extend the envelope by it.
            deadline += delay
        elif _transient(stderr):
            if not retry_transient:
                raise GitHubTransientError(
                    f'GitHub GET transient failure (gh exit {result.returncode})')
            delay = _BACKOFF_SECONDS[attempt] if attempt < len(_BACKOFF_SECONDS) else None
        if delay is None or time.monotonic() + delay >= deadline:
            raise GitHubReadError(
                f'GitHub GET failed after {attempt + 1} attempt(s) '
                f'(gh exit {result.returncode})')
        print(f'CI_GITHUB_READ_RETRY attempt={attempt + 1}/4 delay={delay}s',
              file=sys.stderr, flush=True)
        _sleep(delay)


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
