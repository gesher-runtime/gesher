#!/usr/bin/env python3
"""Check that gesher-app's WifiManager provides every public SDK member.

Compares `javap -public` of android.net.wifi.WifiManager and each of its public nested
types in the platform android.jar with the same classes in the built app classes. Every
SDK class declaration, field, constructor and method (with its exact descriptor and
static/abstract/final modifiers) must be present; extra hidden members are allowed.
Constant values are compared with `javap -constants`. A missing member is a
NoSuchMethodError/NoSuchFieldError in an app, so any difference fails the check.
"""
import argparse
import re
import subprocess
import sys
import zipfile
from pathlib import Path

OUTER = 'android.net.wifi.WifiManager'


def javap(java_home, classpath, names):
    tool = Path(java_home) / 'bin' / 'javap'
    result = subprocess.run([str(tool), '-public', '-constants', '-cp', str(classpath), *names],
                            check=True, capture_output=True, text=True)
    return result.stdout


def members(text):
    """Map class name -> (declaration, set of member lines)."""
    classes = {}
    current = None
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith('Compiled from'):
            continue
        header = re.match(r'(public .*?(?:class|interface) (\S+?))(?: extends .*?)?(?: implements .*?)? \{$', line)
        if header:
            current = header.group(2)
            classes[current] = (header.group(1), set())
            continue
        if line == '}':
            current = None
            continue
        if current is not None:
            classes[current][1].add(line.rstrip(';'))
    return classes


def nested(jar):
    with zipfile.ZipFile(jar) as archive:
        prefix = OUTER.replace('.', '/')
        return sorted(name[:-6].replace('/', '.') for name in archive.namelist()
                      if name.startswith(prefix + '$') and name.endswith('.class')
                      and not re.search(r'\$\d', name))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home', required=True, type=Path)
    parser.add_argument('--sdk-jar', required=True, type=Path)
    parser.add_argument('--app-classes', required=True, type=Path)
    args = parser.parse_args()
    names = [OUTER] + nested(args.sdk_jar)
    sdk = members(javap(args.java_home, args.sdk_jar, names))
    # javap reports what it cannot find on stderr and fails: run each class separately
    # so one missing nested type is reported by name.
    problems = []
    for name in names:
        try:
            ours = members(javap(args.java_home, args.app_classes, [name]))
        except subprocess.CalledProcessError:
            problems.append('missing class ' + name)
            continue
        declaration, expected = sdk[name]
        if name not in ours:
            problems.append('missing class ' + name)
            continue
        actual_declaration, actual = ours[name]
        if declaration != actual_declaration:
            problems.append('%s declared as %r, SDK %r' % (name, actual_declaration, declaration))
        for member in sorted(expected - actual):
            problems.append('%s: missing or different: %s' % (name, member))
    total = sum(len(members_) for _, members_ in sdk.values())
    if problems:
        print('\n'.join(problems), file=sys.stderr)
        print('WifiManager API check failed: %d of %d SDK members/classes differ' % (len(problems), total),
              file=sys.stderr)
        sys.exit(1)
    print('WifiManager API check passed: %d classes, %d public SDK members present' % (len(names), total))


if __name__ == '__main__':
    main()
