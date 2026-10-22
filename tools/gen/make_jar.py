#!/usr/bin/env python3
"""Package dex files and Java resources into a jar.

  make_jar.py <out.jar> <dex dir> [--resources <list> [--resource-dir <dir>]]

<list> holds resource paths, one per line; each is stored under its path
relative to the nearest `src/main/java/` or `src/main/resources/` directory,
as Soong's java_resources with `path:` does, or relative to <dir> when given,
as with java_resource_dirs.
"""
import os
import re
import sys
import zipfile


def main():
    out, dex = sys.argv[1], sys.argv[2]
    resources, resource_dir = [], None
    if len(sys.argv) > 4 and sys.argv[3] == '--resources':
        resources = [l.strip() for l in open(sys.argv[4], encoding='utf-8') if l.strip()]
    if len(sys.argv) > 6 and sys.argv[5] == '--resource-dir':
        resource_dir = sys.argv[6]
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        for f in sorted(os.listdir(dex)):
            if f.endswith('.dex'):
                z.write(os.path.join(dex, f), f)
        for r in resources:
            if resource_dir is not None:
                z.write(r, os.path.relpath(r, resource_dir).replace(os.sep, '/'))
                continue
            m = re.search(r'/src/main/(?:java|resources)/(.+)$', r.replace(os.sep, '/'))
            if not m:
                raise SystemExit(f'cannot place resource {r}')
            z.write(r, m.group(1))


if __name__ == '__main__':
    main()
