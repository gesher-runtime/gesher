"""Checks out what a Plan needs, in blobless sparse clones.

Each project's sparse set becomes its .bp files plus the directories of the
files the plan needs, so generators find their neighbours (AIDL imports,
proto imports) as they would in a full tree.
"""
import os
import subprocess
from collections import defaultdict


def sparse_sets(tree, paths, whole_dirs=()):
    by_project = defaultdict(lambda: (defaultdict(set), set()))
    for path in paths:
        project = tree.project_of(path)
        if project is not None:
            rel = path[len(project) + 1:]
            by_project[project][0][os.path.dirname(rel)].add(rel)
    for path in whole_dirs:
        project = tree.project_of(path)
        if project is not None and path != project:
            by_project[project][1].add(path[len(project) + 1:])
    return by_project


def checkout(tree, paths, whole_dirs=()):
    for project, (dirs, wholes) in sorted(sparse_sets(tree, paths, whole_dirs).items()):
        # The files directly in each directory (not its subdirectories), and
        # whole trees where generators search (AIDL include directories).
        # Excluding a directory would keep git from re-including a whole tree
        # under it, so a whole tree's ancestors list their files instead.
        patterns = ['*.bp']
        ancestors = {os.path.dirname(w) for w in wholes}
        for w in wholes:
            while w:
                w = os.path.dirname(w)
                ancestors.add(w)
        for d, files in sorted(dirs.items()):
            if any(d == w or d.startswith(w + '/') for w in wholes):
                continue  # inside a whole tree
            if d in ancestors:
                patterns += [f'/{f}' for f in sorted(files)]
            else:
                prefix = f'/{d}/' if d else '/'
                patterns += [f'{prefix}*', f'!{prefix}*/']
        patterns += [f'/{d}/' for d in sorted(wholes)]
        git = ['git', '-C', os.path.join(tree.root, project)]
        subprocess.run(git + ['sparse-checkout', 'set', '--no-cone', '--stdin'],
                       input='\n'.join(patterns).encode(), check=True)
        subprocess.run(git + ['-c', 'core.autocrlf=false', 'checkout', '-q', 'HEAD'],
                       check=True)
