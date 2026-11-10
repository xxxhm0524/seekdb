#!/usr/bin/env python3
"""Force graph search across float32 underflow/overflow and dense tie ranges."""
from contextlib import ExitStack
import json
import math
from pathlib import Path
import sys
import tempfile

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bench import vector_literal
from protocol_client import Client


def main():
    rng = np.random.default_rng(20261111)
    count, dimension = 2048, 64
    ids = np.arange(1, count + 1, dtype=np.int64)
    reports = []
    with tempfile.TemporaryDirectory(prefix='mvd-ann-range-') as data, ExitStack() as cleanup:
        client = Client(datadir=data, extra=['--cache-mb=1', '--ann-min-rows=2048'])
        cleanup.callback(client.close)

        def sql(text, mode='exact', **fields):
            response = client.request(text, mode)
            assert response.get('ok') is True, (text[:180], response)
            for key, value in fields.items():
                assert response.get(key) == value, (text[:180], response)
            return response

        for exponent in (-135, 0, 110):
            scale = 2.0 ** exponent
            matrix = (rng.uniform(-1, 1, (count, dimension)) * scale).astype(np.float32)
            stored = matrix.astype(np.float64)
            sql('CREATE TABLE t(id BIGINT PRIMARY KEY,cat VARCHAR(8),v VECTOR(64))')
            for start in range(0, count, 100):
                stop = min(start + 100, count)
                values = ','.join("(%d,'%s','%s')" %
                    (i + 1, '\U0001f600' if i % 2 else '\u00e9', vector_literal(matrix[i]))
                    for i in range(start, stop))
                sql('INSERT INTO t VALUES ' + values, affected_rows=stop-start)
            sql('CREATE VECTOR INDEX vi ON t(v)')
            recalls = {'filtered': [], 'unfiltered': []}
            for query_index in range(20):
                query = (rng.uniform(-1, 1, dimension) * scale).astype(np.float32)
                if query_index == 0:
                    query = matrix[-1].copy()  # Also verify a genuine zero distance.
                filtered = bool(query_index % 2)
                candidates = np.flatnonzero(ids % 2 == 0) if filtered else np.arange(count)
                delta = stored[candidates] - query.astype(np.float64)
                squared = np.einsum('ij,ij->i', delta, delta)
                truth = ids[candidates[np.lexsort((ids[candidates], squared))[:10]]].tolist()
                where = " WHERE cat='\U0001f600'" if filtered else ''
                statement = ("SELECT id,L2_DISTANCE(v,'%s') AS d FROM t%s ORDER BY d LIMIT 10" %
                             (vector_literal(query), where))
                for mode in ('exact', 'ann'):
                    rows = sql(statement, mode)['rows']
                    got = [row[0] for row in rows]
                    assert len(got) == len(set(got)) == 10, (exponent, mode, got)
                    assert all(1 <= ident <= count and (not filtered or ident % 2 == 0) for ident in got)
                    for ident, distance in rows:
                        expected = float(np.linalg.norm(stored[ident-1] - query.astype(np.float64)))
                        assert math.isfinite(distance) and math.isclose(distance, expected, rel_tol=1e-7, abs_tol=0), (
                            exponent, mode, ident, distance, expected)
                    if mode == 'exact':
                        assert got == truth, (exponent, query_index, got, truth)
                    else:
                        recalls['filtered' if filtered else 'unfiltered'].append(len(set(got) & set(truth)) / 10)
            means = {key: float(np.mean(values)) for key, values in recalls.items()}
            assert all(value >= 0.95 for value in means.values()), (exponent, means)
            reports.append(dict(exponent=exponent, recall_at_10=means))
            sql('DROP TABLE t')

            # Every permutation has exactly the same norm, including when a
            # float accumulator would underflow or overflow. PKs break ties.
            basis = np.array([0, 1, 2, 3] * (dimension // 4), dtype=np.float32) * scale
            sql('CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(64))')
            for start in range(count, 0, -100):
                stop = max(start - 100, 0)
                values = ','.join("(%d,'%s')" % (ident, vector_literal(rng.permutation(basis)))
                                  for ident in range(start, stop, -1))
                sql('INSERT INTO t VALUES ' + values, affected_rows=start-stop)
            sql('CREATE VECTOR INDEX vi ON t(v)')
            zero = vector_literal(np.zeros(dimension, dtype=np.float32))
            for mode in ('exact', 'ann'):
                sql("SELECT id FROM t ORDER BY L2_DISTANCE(v,'%s') LIMIT 10" % zero,
                    mode, rows=[[ident] for ident in range(1, 11)])
                sql("SELECT id FROM t ORDER BY L2_DISTANCE(v,'%s') LIMIT 5 OFFSET 3" % zero,
                    mode, rows=[[ident] for ident in range(4, 9)])
            sql('DROP TABLE t')
        report = dict(passed=True, graph_threshold=count, dimensions=dimension,
                      scales=reports, dense_ties='canonical primary keys in both modes')
        output = Path(__file__).resolve().parent.parent / 'protocol-results/ann-numeric-range.json'
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        print(json.dumps(report))


if __name__ == '__main__':
    main()
