#!/usr/bin/env python3
"""Run reproducible JSONL regressions and preserve actual logs/verdicts."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--large-resource', action='store_true')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True,exist_ok=False)
    binary = str(ROOT/('minivectordb.exe' if os.name=='nt' else 'minivectordb'))
    command = json.dumps([binary,'--data-dir','{data_dir}'])
    checks = [
        ('handshake',['protocol_check/run.py','--command',command,'--handshake-only','--output',str(output/'handshake')]),
        ('public',['protocol_check/run.py','--command',command,'--output',str(output/'public')]),
        ('extended',['tests/run_extended_tsv.py']),
        ('requirements',['tests/formal_coverage.py']),
        ('hidden-boundaries',['tests/hidden_boundaries.py']),
        ('vector-precision',['tests/vector_precision_model.py']),
        ('resource',['tests/resource_boundaries.py']),
        ('mutation-io',['tests/mutation_io.py']),
        ('spool-read',['tests/spool_read_boundaries.py']),
        ('page-churn',['tests/page_pool_churn.py']),
        ('cache-coherence',['tests/cache_coherence.py']),
        ('transaction-spill',['tests/transaction_spill.py']),
        ('ann-lifecycle',['tests/ann_lifecycle.py']),
        ('regression',['tests/regression.py']),
        ('recovery',['tests/crash_recovery_test.py','50000']),
        ('commit-crash',['tests/commit_crash_boundaries.py']),
    ]
    checks += [('stress-'+str(seed),['tests/stress_random.py','--rounds','100','--seed',str(seed)])
               for seed in (7,42,211)]
    checks += [('bench-'+mode,['tests/bench.py','--search-mode',mode,'--seed',str(seed),
                '--output',str(output/('bench-'+mode+'.json'))])
               for mode,seed in (('exact',20261110),('ann',20261111))]
    if args.large_resource:
        checks += [('resource-2g',['tests/resource_boundaries.py','--rows','260000','--max-mb','128'])]
    env = dict(os.environ,PYTHONUTF8='1',PYTHONIOENCODING='utf-8',MVD_BIN=binary)
    results = []
    for name,argv in checks:
        before = time.perf_counter()
        try:
            process = subprocess.run([sys.executable,*argv],cwd=ROOT,env=env,stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,text=True,encoding='utf-8',errors='replace',timeout=600)
            code,log = process.returncode,process.stdout
        except subprocess.TimeoutExpired as exc:
            code,log = -1,'TEST TIMEOUT\n'+str(exc.stdout or '')
        (output/(name+'.log')).write_text(log,encoding='utf-8')
        result = dict(name=name,exit_code=code,seconds=round(time.perf_counter()-before,3))
        results.append(result)
        (output/'summary.json').write_text(json.dumps(results,indent=2)+'\n',encoding='utf-8')
        print(json.dumps(result),flush=True)
        if code:
            print(log[-3000:],flush=True)
    print('REPORT',output,flush=True)
    return int(any(row['exit_code'] for row in results))


if __name__=='__main__':
    sys.exit(main())
