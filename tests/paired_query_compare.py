#!/usr/bin/env python3
"""Paired JSONL query timings on separately prepared identical collections.

Uses real protocol deadlines. This diagnostic does not produce official T/B/M
scores: the prepared containers are reused across query rounds.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import statistics
import sys
import time

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT/'protocol_check'))
from _transport import Session
from bench import vector_literal


def main():
    parser = argparse.ArgumentParser()
    for version in ('reference', 'candidate'):
        group = parser.add_mutually_exclusive_group(required=True)
        group.add_argument('--'+version+'-image')
        group.add_argument('--'+version+'-binary', type=Path)
    parser.add_argument('--rows', type=int, default=50000)
    parser.add_argument('--dim', type=int, default=128)
    parser.add_argument('--queries', type=int, default=300)
    parser.add_argument('--rounds', type=int, default=3)
    parser.add_argument('--seed', type=int, default=20261112)
    parser.add_argument('--check-default-mode', action='store_true')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    assert args.rows >= 10 and args.queries >= 2 and args.queries % 2 == 0 and args.rounds >= 1
    assert bool(args.reference_image) == bool(args.candidate_image), 'Compare the same transport backend'
    output = args.output; output.parent.mkdir(parents=True, exist_ok=True)
    config = json.loads((ROOT/'protocol_check/config.json').read_text())
    assert config['request_timeout'] == 60 and config['case_timeout'] == 600
    vectors = np.random.default_rng(args.seed).random((args.rows, args.dim), dtype=np.float32)
    queries = np.random.default_rng(args.seed+1).random((args.queries, args.dim), dtype=np.float32)
    ids = np.arange(1, args.rows+1, dtype=np.int64)
    batches = []
    for start in range(0, args.rows, 100):
        stop = min(start+100, args.rows)
        batches.append(('INSERT INTO t VALUES '+','.join("(%d,'c%d','%s')" %
            (i+1, i%10, vector_literal(vectors[i])) for i in range(start, stop)), stop-start))
    sql_queries, truths = [], []
    stored = vectors.astype(np.float64)
    for i, query in enumerate(queries):
        category = i%10 if i < args.queries//2 else None
        eligible = np.flatnonzero((ids-1)%10 == category) if category is not None else np.arange(args.rows)
        delta = stored[eligible]-query.astype(np.float64)
        squared = np.einsum('ij,ij->i', delta, delta)
        truth = ids[eligible[np.lexsort((ids[eligible], squared))[:10]]].tolist()
        where = " WHERE cat='c%d'" % category if category is not None else ''
        sql_queries.append("SELECT id,L2_DISTANCE(v,'%s') AS d FROM t%s ORDER BY d LIMIT 10" %
                           (vector_literal(query), where))
        truths.append((truth, category))
    sessions = {}
    report = dict(passed=False, official_score=False, rows=args.rows, dim=args.dim, queries=args.queries,
                  seed=args.seed, scope='prepared independent sessions, sequential alternating query rounds; no resource score',
                  preparations={}, rounds=[])

    def save():
        output.write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')

    def sql(session, text):
        response = session.request(text, mode='ann')
        assert response.get('ok') is True, (text[:120], response)
        return response

    try:
        for version in ('reference', 'candidate'):
            image = getattr(args, version+'_image'); binary = getattr(args, version+'_binary')
            session = Session(config, output.with_suffix('.'+version+'.stderr.log'), image=image,
                command=None if image else [str(binary.resolve()), '--data-dir', '{data_dir}', '--query-threads=2'])
            sessions[version] = session; session.start()
            sql(session, 'CREATE TABLE t(id BIGINT PRIMARY KEY,cat VARCHAR(8),v VECTOR(%d))' % args.dim)
            for batch, count in batches:
                assert sql(session, batch)['affected_rows'] == count
            before = time.perf_counter(); sql(session, 'CREATE VECTOR INDEX vi ON t(v)')
            report['preparations'][version] = dict(index_seconds=time.perf_counter()-before,
                image=image, binary_sha256=None if image else hashlib.sha256(binary.read_bytes()).hexdigest())
            save()
        warm = list(range(min(10, args.queries//2))) + list(range(args.queries//2, min(args.queries, args.queries//2+10)))
        for round_index in range(args.rounds):
            order = ('reference', 'candidate') if round_index%2 == 0 else ('candidate', 'reference')
            round_result, responses = {}, {}
            for version in order:
                session = sessions[version]
                for i in warm: sql(session, sql_queries[i])
                results = []; before = time.perf_counter(); filtered_seconds = None
                for i, statement in enumerate(sql_queries):
                    results.append(sql(session, statement)['rows'])
                    if i+1 == args.queries//2: filtered_seconds = time.perf_counter()-before
                total = time.perf_counter()-before
                recall = {'filtered': [], 'unfiltered': []}
                for i, rows in enumerate(results):
                    truth, category = truths[i]; got = [row[0] for row in rows]
                    assert len(got) == len(set(got)) == len(truth)
                    assert all(1 <= key <= args.rows and (category is None or (key-1)%10 == category) for key in got)
                    for key, distance in rows:
                        expected = float(np.linalg.norm(stored[key-1]-queries[i].astype(np.float64)))
                        assert math.isfinite(distance) and math.isclose(distance, expected, rel_tol=1e-7, abs_tol=1e-7)
                    if truth: recall['filtered' if category is not None else 'unfiltered'].append(len(set(got)&set(truth))/len(truth))
                means = {key: float(np.mean(values)) if values else 1.0 for key, values in recall.items()}
                assert min(means.values()) >= 0.95, means
                round_result[version] = dict(seconds=total, filtered_seconds=filtered_seconds,
                    unfiltered_seconds=total-filtered_seconds, recall_at_10=means)
                responses[version] = results
            assert responses['reference'] == responses['candidate'], 'Optimization changed query results'
            round_result['all_results_identical'] = True
            report['rounds'].append(round_result); save(); print(json.dumps(round_result), flush=True)
        report['medians'] = {version: {key: statistics.median(r[version][key] for r in report['rounds'])
            for key in ('seconds', 'filtered_seconds', 'unfiltered_seconds')} for version in sessions}
        if args.check_default_mode:
            # Prefer an observed ANN/exact difference, so the protocol probe
            # can actually catch a mode leaking from the previous request.
            probe = next((i for i, rows in enumerate(responses['candidate'])
                          if [row[0] for row in rows] != truths[i][0]), 0)
            session = sessions['candidate']
            exact = session.request(sql_queries[probe], mode='exact')
            assert exact.get('ok') is True and [r[0] for r in exact['rows']] == truths[probe][0], exact
            ann = sql(session, sql_queries[probe])
            session.sequence += 1
            ident = session.sequence
            session.send(dict(id=ident, op='execute', sql=sql_queries[probe]))
            omitted = session.read(config['request_timeout'])
            check = dict(probe=probe, exact_ids=[r[0] for r in exact['rows']],
                         ann_ids=[r[0] for r in ann['rows']], omitted_ids=[r[0] for r in omitted.get('rows', [])],
                         observed_ann_difference=ann['rows'] != exact['rows'])
            report['mode_default_check'] = check
            save()
            assert omitted.get('id') == ident and omitted.get('ok') is True and omitted.get('rows') == exact['rows'], \
                ('omitted mode after ANN did not default to exact', check)
            check['passed'] = True
        report['passed'] = True
    except Exception as error:
        report['error'] = type(error).__name__+': '+str(error)
        raise
    finally:
        for session in sessions.values():
            try:
                session.close()
            except Exception as error:
                report['passed'] = False
                report.setdefault('cleanup_errors', []).append(type(error).__name__)
        save()
        if report.get('cleanup_errors'):
            raise RuntimeError('Paired query sessions did not all close cleanly')
    print(json.dumps(dict(passed=True, medians=report['medians'], official_score=False)), flush=True)


if __name__ == '__main__':
    main()
