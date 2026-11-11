#!/usr/bin/env python3
"""Large OFFSET with a small result must not retain every wide output value."""
import argparse
from contextlib import ExitStack
import json
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from protocol_client import Client
from resource_boundaries import peak_bytes


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--rows', type=int, default=12000)
    parser.add_argument('--max-mb', type=float, default=48)
    args = parser.parse_args()
    assert args.rows >= 10 and args.max_mb > 0
    dense = '\U0001f600' * 2048
    with tempfile.TemporaryDirectory(prefix='mvd-projection-memory-') as data, ExitStack() as cleanup:
        client = Client(datadir=data, extra=['--cache-mb=1', '--tx-memory-mb=1'])
        cleanup.callback(client.close)

        def sql(text, **expected):
            response = client.request(text)
            assert response.get('ok') is True, (text[:100], response)
            for key, value in expected.items():
                assert response.get(key) == value, (key, text[:100], response)
            return response

        sql('CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(2048),v VECTOR(2))')
        for start in range(0, args.rows, 100):
            stop = min(start+100, args.rows)
            sql('INSERT INTO t VALUES '+','.join("(%d,'%s','[%d,0]')" % (i, dense, i)
                for i in range(start, stop)), affected_rows=stop-start)
        client.shutdown(); client.start()
        baseline = peak_bytes(client.p.pid)
        sql('SELECT * FROM t ORDER BY id LIMIT 1 OFFSET %d' % (args.rows-1),
            columns=['id', 's', 'v'], rows=[[args.rows-1, dense, [args.rows-1, 0]]])
        peak = peak_bytes(client.p.pid)
        assert peak < args.max_mb*1024*1024, ('wide projected values retained before OFFSET',
            dict(peak_bytes=peak, baseline_bytes=baseline, max_mb=args.max_mb,
                 effective_text_bytes=args.rows*8192, returned_rows=1))
        sql("SELECT id,s,v,L2_DISTANCE(v,'[0,0]') AS d FROM t ORDER BY d DESC LIMIT 1 OFFSET %d"
            % (args.rows-1), columns=['id', 's', 'v', 'd'], rows=[[0, dense, [0, 0], 0]])
        sql('BEGIN')
        sql("UPDATE t SET s='changed',v='[-1,0]' WHERE id=%d" % (args.rows-1), affected_rows=1)
        sql("INSERT INTO t VALUES (%d,'pending','[%d,0]')" % (args.rows, args.rows), affected_rows=1)
        sql('SELECT id,s,v FROM t ORDER BY id LIMIT 1 OFFSET %d' % args.rows,
            rows=[[args.rows, 'pending', [args.rows, 0]]])
        sql('SELECT id,s,v FROM t ORDER BY id DESC LIMIT 1 OFFSET 1',
            rows=[[args.rows-1, 'changed', [-1, 0]]])
        sql('ROLLBACK')
        sql('SELECT s FROM t WHERE id=%d' % (args.rows-1), rows=[[dense]])
        sql('SELECT id,s FROM t ORDER BY id LIMIT 1 OFFSET %d' % args.rows, rows=[])
        peak = peak_bytes(client.p.pid)
        assert peak < args.max_mb*1024*1024, ('transaction projection exceeded memory budget', peak)
        print(json.dumps(dict(passed=True, rows=args.rows, effective_text_bytes=args.rows*8192,
            baseline_bytes=baseline, peak_bytes=peak, max_mb=args.max_mb,
            scalar_and_distance_order=True, pending_insert_update_and_rollback=True)))


if __name__ == '__main__':
    main()
