#!/usr/bin/env python3
"""Verify the ordered patch stack without changing a kernel worktree or its index.

Needs Python 3 and Git only. Replayed blobs/tree objects may be added to Git's
object store. No branches, tags, commits, or checked-out files are changed.
SHA256SUMS checks integrity, not the authenticity of an untrusted download.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def verify(kernel, bundle):
    checksums = {}
    for line in (bundle / 'SHA256SUMS').read_text().splitlines():
        digest, name = line.split('  ', 1)
        relative = Path(name)
        if relative.is_absolute() or '..' in relative.parts or name in checksums:
            raise ValueError(f'Unsafe or duplicate checksum path: {name}')
        if len(digest) != 64 or any(c not in '0123456789abcdef' for c in digest):
            raise ValueError(f'Invalid SHA256 for {name}')
        actual = hashlib.sha256((bundle / relative).read_bytes()).hexdigest()
        if actual != digest:
            raise ValueError(f'Checksum mismatch: {name}')
        checksums[name] = digest
    for required in ('metadata.json', 'series', 'config/zorn.config'):
        if required not in checksums:
            raise ValueError(f'Missing checksum: {required}')

    meta = json.loads((bundle / 'metadata.json').read_text())
    names = [line.strip() for line in (bundle / 'series').read_text().splitlines()
             if line.strip() and not line.lstrip().startswith('#')]
    if not names or len(names) != len(set(names)):
        raise ValueError('Empty series or duplicate patch entries')
    for name in names:
        if name not in checksums or not name.startswith('patches/') or not name.endswith('.patch'):
            raise ValueError(f'Patch is not explicitly checksummed: {name}')
    if set(names) != {n for n in checksums if n.startswith('patches/') and n.endswith('.patch')}:
        raise ValueError('Series does not include every checksummed patch')

    with tempfile.TemporaryDirectory(prefix='zorn-patch-verify-') as temporary:
        env = os.environ.copy()
        env['GIT_INDEX_FILE'] = str(Path(temporary) / 'index')

        def git(*args):
            result = subprocess.run(['git', '-C', str(kernel), *args], env=env,
                                    text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            if result.returncode:
                raise RuntimeError(result.stderr.strip() or f'git {args[0]} failed')
            return result.stdout.strip()

        base = git('rev-parse', '--verify', meta['base_commit'] + '^{commit}')
        if base != meta['base_commit'] or git('rev-parse', base + '^{tree}') != meta['base_tree']:
            raise ValueError('Pristine base commit/tree does not match metadata')
        git('read-tree', base)
        for name in names:
            patch = str(bundle / name)
            git('apply', '--cached', '--check', '--whitespace=nowarn', patch)
            git('apply', '--cached', '--whitespace=nowarn', patch)
            print(f'APPLIED {name}')
        tree = git('write-tree')
        if tree != meta['snapshot_tree']:
            raise ValueError(f'Replayed tree mismatch: got {tree}, expected {meta["snapshot_tree"]}')
        print(f'PASS: {len(names)} patches; pristine base {base}; resulting tree {tree}')
        print('The kernel worktree, real index, HEAD and refs were not modified.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kernel', type=Path, help='Git kernel checkout containing the pristine base commit')
    args = parser.parse_args()
    try:
        verify(args.kernel.resolve(), Path(__file__).resolve().parent)
    except (OSError, ValueError, KeyError, RuntimeError) as exc:
        print(f'FAIL: {exc}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
