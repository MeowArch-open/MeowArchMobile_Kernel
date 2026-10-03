#!/usr/bin/env python3
"""Real-stack smoke checks: python3 test-verifier.py /path/to/linux

Exercises success and four failure cases using the actual patch files and Git,
without mocking either. Needs the base kernel commit locally, but no compiler.
"""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def checksum(bundle):
    files = [p for p in sorted(bundle.rglob('*'))
             if p.is_file() and p.name != 'SHA256SUMS'
             and '.git' not in p.parts and '__pycache__' not in p.parts]
    (bundle / 'SHA256SUMS').write_text(''.join(
        hashlib.sha256(p.read_bytes()).hexdigest() + '  ' + str(p.relative_to(bundle)) + '\n'
        for p in files))


def main():
    if len(sys.argv) != 2:
        raise SystemExit('usage: python3 test-verifier.py /path/to/linux')
    kernel = Path(sys.argv[1]).resolve()
    bundle = Path(__file__).resolve().parent

    def git(*args):
        return subprocess.check_output(['git', '-C', str(kernel), *args])

    def state():
        index = Path(git('rev-parse', '--path-format=absolute', '--git-path', 'index').decode().strip())
        return (git('rev-parse', 'HEAD'), hashlib.sha256(index.read_bytes()).hexdigest(),
                git('status', '--porcelain'))

    def run(candidate, code, message):
        before = state()
        result = subprocess.run(['python3', str(candidate / 'verify-series.py'), str(kernel)],
                                capture_output=True, text=True)
        if state() != before:
            raise AssertionError('Verifier changed kernel HEAD, real index or worktree status')
        if result.returncode != code or message not in result.stdout + result.stderr:
            raise AssertionError((result.returncode, result.stdout, result.stderr))
        print('PASS:', message)

    run(bundle, 0, 'PASS: 6 patches')
    with tempfile.TemporaryDirectory(prefix='zorn-verifier-test-') as temporary:
        candidate = Path(temporary) / 'bundle'
        shutil.copytree(bundle, candidate, ignore=shutil.ignore_patterns('.git', '__pycache__'))
        series = (candidate / 'series').read_text()
        patch = candidate / series.splitlines()[0]
        original = patch.read_bytes()
        patch.write_bytes(original + b'\ncorruption\n')
        run(candidate, 1, 'Checksum mismatch: patches/')
        patch.write_bytes(original)
        (candidate / 'series').write_text(series.splitlines()[0] + '\n' + series)
        checksum(candidate)
        run(candidate, 1, 'duplicate patch entries')
        (candidate / 'series').write_text(series)
        metadata = json.loads((candidate / 'metadata.json').read_text())
        metadata['snapshot_tree'] = '0' * 40
        (candidate / 'metadata.json').write_text(json.dumps(metadata))
        checksum(candidate)
        run(candidate, 1, 'Replayed tree mismatch')
        metadata['base_commit'] = '0' * 40
        (candidate / 'metadata.json').write_text(json.dumps(metadata))
        checksum(candidate)
        run(candidate, 1, 'FAIL:')
    print('5 checks passed; real kernel HEAD, index and status unchanged.')


if __name__ == '__main__':
    main()
