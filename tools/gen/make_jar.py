#!/usr/bin/env python3
"""Package dex files and Java resources into a jar.

  make_jar.py <out.jar> <dex dir> [--resources <list>]

<list> holds resource paths, one per line; each is stored under its path
relative to the nearest `src/main/java/` or `src/main/resources/` directory,
as Soong's java_resources with `path:` does.
"""
import os
import re
import sys
import zipfile


def main():
    out, dex = sys.argv[1], sys.argv[2]
    resources = []
    if len(sys.argv) > 4 and sys.argv[3] == '--resources':
        resources = [l.strip() for l in open(sys.argv[4], encoding='utf-8') if l.strip()]
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        for f in sorted(os.listdir(dex)):
            if f.endswith('.dex'):
                z.write(os.path.join(dex, f), f)
        for r in resources:
            m = re.search(r'/src/main/(?:java|resources)/(.+)$', r.replace(os.sep, '/'))
            if not m:
                raise SystemExit(f'cannot place resource {r}')
            z.write(r, m.group(1))


if __name__ == '__main__':
    main()
