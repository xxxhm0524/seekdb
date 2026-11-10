#!/usr/bin/env python3
"""Repeated wide scans stay bounded when prefetch and eviction use full batches."""
from contextlib import ExitStack
import json
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from protocol_client import Client
from resource_boundaries import peak_bytes


def main():
    with tempfile.TemporaryDirectory(prefix='mvd-page-churn-') as data, ExitStack() as cleanup:
        client = Client(datadir=data, extra=['--cache-mb=8'])
        cleanup.callback(client.close)
        def sql(text, **fields):
            response = client.request(text)
            assert response.get('ok') is True, (text[:100], response)
            for key, value in fields.items():
                assert response.get(key) == value, (text[:100], response)
            return response
        rows = 24000
        sql('CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(2048),v VECTOR(2))')
        for start in range(0, rows, 100):
            sql('INSERT INTO t VALUES '+','.join("(%d,'x','[%d,0]')"%(i, i)
                for i in range(start, start+100)), affected_rows=100)
        peak = peak_bytes(client.p.pid)
        client.kill(); client.start()
        for turn in range(6):
            sql('SELECT id FROM t ORDER BY id DESC LIMIT 3', rows=[[rows-1], [rows-2], [rows-3]])
            peak = max(peak, peak_bytes(client.p.pid))
            assert peak < 80*2**20, ('scan allocator growth', turn, peak)
        print(json.dumps(dict(passed=True, rows=rows, scans=6, peak_bytes=peak)))


if __name__ == '__main__':
    main()
