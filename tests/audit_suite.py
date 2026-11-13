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
        ('header-contracts',['tests/header_self_containment.py','--output',str(output/'header-contracts.json')]),
        ('json-nesting',['tests/json_nesting_boundaries.py','--output',str(output/'json-nesting.json')]),
        ('startup-io-failure',['tests/startup_io_failure.py','--output',str(output/'startup-io-failure.json')]),
        ('file-io-progress',['tests/file_io_progress.py','--output',str(output/'file-io-progress.json')]),
        ('parallel-query-failure',['tests/parallel_query_failure.py','--output',str(output/'parallel-query-failure.json')]),
        ('sql-comment-boundaries',['tests/sql_comment_boundaries.py','--output',str(output/'sql-comment-boundaries.json')]),
        ('vector-token-types',['tests/vector_token_types.py','--output',str(output/'vector-token-types.json')]),
        ('identifier-boundaries',['tests/identifier_boundaries.py','--output',str(output/'identifier-boundaries.json')]),
        ('sqlite-differential',['tests/sqlite_differential.py','--output',str(output/'sqlite-differential.json')]),
        ('vector-literal-syntax',['tests/vector_literal_syntax.py','--output',str(output/'vector-literal-syntax.json')]),
        ('returned-distance-order',['tests/returned_distance_order.py','--output',str(output/'returned-distance-order.json')]),
        ('exact-norm-boundary',['tests/exact_norm_boundary.py','--output',str(output/'exact-norm-boundary.json')]),
        ('exact-norm-model',['tests/exact_norm_model.py','--output',str(output/'exact-norm-model.json')]),
        ('state-machine-model',['tests/state_machine_model.py','--seed','20261113','--steps','1200',
            '--output',str(output/'state-machine-model.json')]),
        ('requirements',['tests/formal_coverage.py']),
        ('refactor-contracts',['tests/refactor_contracts.py','--output',str(output/'refactor-contracts.json')]),
        ('ddl-failure-atomicity',['tests/create_sync_failure.py','--output',str(output/'ddl-failure-atomicity.json')]),
        ('transaction-allocation-failure',['tests/transaction_allocation_failure.py','--output',str(output/'transaction-allocation-failure.json')]),
        ('commit-allocation-failure',['tests/commit_allocation_failure.py','--output',str(output/'commit-allocation-failure.json')]),
        ('spool-rewind-failure',['tests/spool_rewind_failure.py','--output',str(output/'spool-rewind-failure.json')]),
        ('wide-spill-state-model',['tests/state_machine_model.py','--wide-spill','--seed','20261202','--steps','1200','--output',str(output/'wide-state-machine-model.json')]),
        ('hidden-boundaries',['tests/hidden_boundaries.py']),
        ('vector-precision',['tests/vector_precision_model.py']),
        ('indexed-vector-precision',['tests/vector_precision_model.py','--indexed']),
        ('float-parser',['tests/float_parse_model.py']),
        ('heap-rejection',['tests/heap_rejection_model.py']),
        ('filter-cache',['tests/filter_cache_model.py']),
        ('material-boundaries',['tests/test_agent_material.py']),
        ('material',['tests/validate_agent_material.py','--path','agent/conversation.json',
            '--mirror','conversation.json','--require-evidence']),
        ('resource',['tests/resource_boundaries.py']),
        ('mutation-io',['tests/mutation_io.py']),
        ('wal-read-cache',['tests/wal_read_model.py']),
        ('page-image-codec',['tests/page_image_codec_model.py']),
        ('compact-image-io',['tests/compact_image_io.py']),
        ('compact-cache-model',['tests/compact_cache_model.py']),
        ('compact-read-cache',['tests/compact_read_cache.py','--output',str(output/'compact-read-cache.json')]),
        ('sparse-io',['tests/sparse_io_model.py']),
        ('sparse-storage',['tests/sparse_storage.py','--expect-sparse','--output',str(output/'sparse-storage.json')]),
        ('primary-key-projection',['tests/primary_key_projection.py']),
        ('projection-memory',['tests/projection_memory.py']),
        ('external-sort-model',['tests/external_sort_model.py']),
        ('ordered-key-memory',['tests/ordered_key_memory.py','--output',str(output/'ordered-key-memory.json')]),
        ('spool-read',['tests/spool_read_boundaries.py']),
        ('page-churn',['tests/page_pool_churn.py']),
        ('cache-coherence',['tests/cache_coherence.py']),
        ('transaction-spill',['tests/transaction_spill.py']),
        ('ann-lifecycle',['tests/ann_lifecycle.py']),
        ('ann-numeric-range',['tests/ann_numeric_range.py']),
        ('ann-index-deadline',['tests/ann_index_deadline.py','--output',str(output/'ann-index-deadline.json')]),
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
