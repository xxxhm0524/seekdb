#!/usr/bin/env python3
"""Real 60-second JSONL index deadlines near float32 distance underflow."""
import argparse
import json
import math
import os
from pathlib import Path
import sys
import time

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tests'))
sys.path.insert(0, str(ROOT / 'protocol_check'))
from _transport import Session
from bench import vector_literal


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--image')
    parser.add_argument('--rows', type=int, default=50000)
    parser.add_argument('--exponents', type=int, nargs='+', default=[-75, 0])
    parser.add_argument('--output', type=Path, default=ROOT / 'protocol-results/ann-index-deadline.json')
    args = parser.parse_args()
    assert args.rows >= 40000, 'Keep the default graph activation threshold covered'
    output = args.output
    output.parent.mkdir(parents=True, exist_ok=True)
    config = json.loads((ROOT / 'protocol_check/config.json').read_text())
    assert config['request_timeout'] == 60
    binary = os.environ.get('MVD_BIN') or str(ROOT / ('minivectordb.exe' if os.name == 'nt' else 'minivectordb'))
    session = Session(config, output.with_suffix('.stderr.log'), image=args.image,
                      command=None if args.image else [binary, '--data-dir', '{data_dir}'])
    started = time.perf_counter()
    report = dict(passed=False, rows=args.rows, dimensions=64,
                  request_timeout_seconds=config['request_timeout'], stages=[])

    def save():
        report['total_seconds'] = time.perf_counter() - started
        output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')

    def sql(text, mode='exact', **fields):
        result = session.request(text, mode)
        assert result.get('ok') is True, (text[:100], result)
        for key, value in fields.items():
            assert result.get(key) == value, (text[:100], result)
        return result

    before = started
    try:
        session.start()
        for exponent in args.exponents:
            sql('CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(64))')
            rng = np.random.default_rng(20261111)
            matrix = (rng.uniform(-1, 1, (args.rows, 64)) * 2.0 ** exponent).astype(np.float32)
            before = time.perf_counter()
            for start in range(0, args.rows, 100):
                stop = min(start + 100, args.rows)
                values = ','.join("(%d,'%s')" % (i+1, vector_literal(matrix[i])) for i in range(start, stop))
                sql('INSERT INTO t VALUES ' + values, affected_rows=stop-start)
            step = dict(exponent=exponent, stage='load', seconds=time.perf_counter()-before)
            report['stages'].append(step); save(); print(json.dumps(step), flush=True)
            before = time.perf_counter()
            report['stage'] = 'CREATE VECTOR INDEX'; report['exponent'] = exponent; save()
            sql('CREATE VECTOR INDEX vi ON t(v)')
            step = dict(exponent=exponent, stage='index', seconds=time.perf_counter()-before)
            report['stages'].append(step); save(); print(json.dumps(step), flush=True)
            report['stage'] = 'ANN recall'
            ids = np.arange(1, args.rows+1)
            stored = matrix.astype(np.float64)
            recalls = []
            for _ in range(20):
                query = (rng.uniform(-1, 1, 64) * 2.0 ** exponent).astype(np.float32)
                delta = stored-query.astype(np.float64)
                distances = np.einsum('ij,ij->i', delta, delta)
                truth = ids[np.lexsort((ids, distances))[:10]].tolist()
                result = sql("SELECT id,L2_DISTANCE(v,'%s') AS d FROM t ORDER BY d LIMIT 10" %
                             vector_literal(query), mode='ann')
                got = [row[0] for row in result['rows']]
                assert len(got) == len(set(got)) == 10 and all(1 <= i <= args.rows for i in got), got
                for ident, distance in result['rows']:
                    expected = float(np.linalg.norm(stored[ident-1]-query.astype(np.float64)))
                    assert math.isfinite(distance) and math.isclose(distance, expected, rel_tol=1e-7, abs_tol=0), (
                        exponent, ident, distance, expected)
                recalls.append(len(set(got) & set(truth)) / 10)
            mean_recall = float(np.mean(recalls))
            assert mean_recall >= 0.95, (exponent, mean_recall)
            step = dict(exponent=exponent, stage='recall', mean_recall_at_10=mean_recall)
            report['stages'].append(step); save(); print(json.dumps(step), flush=True)
            if exponent == -75:
                before = time.perf_counter()
                report['stage'] = 'crash and startup graph rebuild'; save()
                session.crash(); session.start()
                sql("SELECT id,L2_DISTANCE(v,'%s') AS d FROM t ORDER BY d LIMIT 1" %
                    vector_literal(matrix[0]), rows=[[1, 0]])
                step = dict(exponent=exponent, stage='crash recovery and rebuilt-index exact query',
                            seconds=time.perf_counter()-before)
                report['stages'].append(step); save(); print(json.dumps(step), flush=True)
            sql('DROP TABLE t')
        session.shutdown(); report['passed'] = True
    except Exception as error:
        report['error'] = type(error).__name__ + ': ' + str(error)
        report['failed_stage_seconds'] = time.perf_counter()-before
    finally:
        session.close(); save()
    print(json.dumps(report), flush=True)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
