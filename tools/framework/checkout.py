"""Checks out what a Plan needs, in blobless sparse clones.

Each project's sparse set gains the files the plan needs, by exact path, and
whole trees where generators search (AIDL include directories). Patterns
are only ever added, so sources other builds checked out (tools/host's)
stay, and no pattern excludes a directory, which would keep git from
re-including anything under it.
"""
import os
import subprocess
from collections import defaultdict


def sparse_sets(tree, paths, whole_dirs=()):
    by_project = defaultdict(lambda: (set(), set()))
    for path in paths:
        project = tree.project_of(path)
        if project is not None:
            by_project[project][0].add(path[len(project) + 1:])
    for path in whole_dirs:
        project = tree.project_of(path)
        if project is not None and path != project:
            by_project[project][1].add(path[len(project) + 1:])
    return by_project


def checkout(tree, paths, whole_dirs=()):
    for project, (files, wholes) in sorted(sparse_sets(tree, paths, whole_dirs).items()):
        git = ['git', '-C', os.path.join(tree.root, project)]
        sparse = subprocess.run(git + ['config', '--bool', 'core.sparseCheckout'],
                                capture_output=True, text=True).stdout.strip()
        if sparse != 'true':
            continue  # a full checkout already has everything
        existing = [p for p in subprocess.run(git + ['sparse-checkout', 'list'],
                                              capture_output=True, text=True).stdout.split('\n') if p]
        wanted = ['*.bp'] + [f'/{d}/' for d in sorted(wholes)]
        wanted += [f'/{f}' for f in sorted(files)
                   if not any(f.startswith(w + '/') for w in wholes)]
        new = [p for p in wanted if p not in set(existing)]
        if not new:
            continue
        subprocess.run(git + ['sparse-checkout', 'set', '--no-cone', '--stdin'],
                       input='\n'.join(existing + new).encode(), check=True)
        subprocess.run(git + ['-c', 'core.autocrlf=false', 'checkout', '-q', 'HEAD'],
                       check=True)
