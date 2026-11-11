#!/usr/bin/env python3
"""Compressible commit images should not require reading the WAL back in full."""
import argparse
from contextlib import ExitStack
import json
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from protocol_client import Client
from mutation_io import read_bytes
from resource_boundaries import peak_bytes


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--rows', type=int, default=12000)
    args = parser.parse_args()
    assert args.rows >= 1000
    dense = '\U0001f600' * 2048
    with tempfile.TemporaryDirectory(prefix='mvd-compact-image-') as data, ExitStack() as cleanup:
        client = Client(datadir=data, extra=['--cache-mb=1', '--tx-memory-mb=1'])
        cleanup.callback(client.close)

        def sql(text, **expected):
            response = client.request(text)
            assert response.get('ok') is True, (text[:100], response)
            for key, value in expected.items():
                assert response.get(key) == value, (key, response)
            return response

        sql('CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(2048),v VECTOR(2))')
        sql('BEGIN')
        for start in range(0, args.rows, 100):
            stop = min(start+100, args.rows)
            sql('INSERT INTO t VALUES '+','.join("(%d,'%s','[%d,0]')" % (i, dense, i)
                for i in range(start, stop)), affected_rows=stop-start)
        spool = (Path(data)/'txn.tmp').stat().st_size
        before = read_bytes(client.p.pid)
        sql('COMMIT')
        reads = read_bytes(client.p.pid)-before
        assert reads < spool+4*1024*1024, ('compressible images were reread from WAL',
            dict(commit_read_bytes=reads, spool_bytes=spool, allowance_bytes=4*1024*1024))
        peak = peak_bytes(client.p.pid)
        client.kill(); client.start()
        for key in (0, 1, args.rows//2, args.rows-1):
            sql('SELECT id,s,v FROM t WHERE id=%d' % key, rows=[[key, dense, [key, 0]]])
        print(json.dumps(dict(passed=True, rows=args.rows, commit_read_bytes=reads,
            spool_bytes=spool, peak_bytes=peak, acknowledged_data_recovered=True)))


if __name__ == '__main__':
    main()
