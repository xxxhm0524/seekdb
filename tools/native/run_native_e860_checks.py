"""Run the unchanged e860 assertions against an optimized native ELF.

Fault-injection tests use the dynamic build so LD_PRELOAD genuinely intercepts
syscalls. Public/container-boundary checks run separately with the hardened
default-entrypoint launcher. This does not claim a Docker build or formal score.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time


def run(a):
    root = Path(__file__).resolve().parent
    source = root / a.source
    binary = (root / a.binary).resolve(strict=True)
    out = root / a.output; out.mkdir(exist_ok=False)
    # The original assertion files expect the frozen public fixture beside them.
    if not (source / 'mysqltest').exists(): shutil.copytree(root / 'official-kit/mysqltest', source / 'mysqltest')
    env = dict(os.environ, MVD_BIN=str(binary), PYTHONUNBUFFERED='1')
    env.pop('MVD_DOCKER_IMAGE', None)
    cases = [(name, []) for name in [
        'protocol_complete_input', 'protocol_ids', 'protocol_contract', 'protocol_nested_ids',
        'sql_token_kinds', 'published_requirements', 'sql_numeric_forms', 'order_alias_boundaries',
        'invariants', 'space_reuse', 'column_layouts', 'cache_column_sets', 'cache_ranges',
        'insert_batches', 'sql_literals', 'cache_unrelated_text', 'crash_commit', 'commit_publish',
        'open_safety', 'compaction_crash', 'recovery_boundaries', 'recovery_allocation',
        'projection_io', 'response_failure']]
    cases += [(name, ['--reference-bin', str(root / 'build/reference-candidate')]) for name in ['reference_boundaries', 'reference_response_budget']]
    rows = []
    for name, extra in cases:
        start = time.monotonic()
        command = [sys.executable, str(source / 'tests' / (name + '.py')), '--output', str(out / name)] + extra
        with (out / (name + '.log')).open('wb') as log:
            result = subprocess.run(command, cwd=source, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=600)
        report = out / name / 'report.json'
        payload = json.loads(report.read_text()) if report.exists() else None
        row = dict(name=name, returncode=result.returncode, passed=result.returncode == 0 and isinstance(payload, dict) and payload.get('passed') is True,
            seconds=time.monotonic() - start, command=command, report=str(report), assertion_sha256=hashlib.sha256((source / 'tests' / (name + '.py')).read_bytes()).hexdigest())
        rows.append(row)
        summary = dict(passed=all(x['passed'] for x in rows), completed=len(rows), expected=len(cases), binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(), results=rows,
            scope='Unchanged test assertions; native dynamic ELF. Boundary constraints and production static ELF have separate evidence. No Docker or formal evaluation is claimed.')
        (out / 'report.json').write_text(json.dumps(summary, indent=2) + '\n')
        print(json.dumps(row), flush=True)
    return all(x['passed'] for x in rows)


if __name__ == '__main__':
    p = argparse.ArgumentParser(); p.add_argument('--source', default='combined-a-source'); p.add_argument('--binary', default='build/combined-a-dynamic'); p.add_argument('--output', default='native-evidence/combined-a-regression')
    raise SystemExit(0 if run(p.parse_args()) else 1)
