#!/usr/bin/env python3
"""Primary-key ordering must not read wide payloads; visibility stays exact."""
from contextlib import ExitStack
import json
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from protocol_client import Client
from mutation_io import read_bytes


def main():
    with tempfile.TemporaryDirectory(prefix='mvd-pk-projection-') as data, ExitStack() as cleanup:
        client = Client(datadir=data, extra=['--cache-mb=1', '--tx-memory-mb=1'])
        cleanup.callback(client.close)
        def sql(text, **expected):
            response = client.request(text)
            assert response.get('ok') is True, (text[:100], response)
            for field, value in expected.items():
                assert response.get(field) == value, (text[:100], response)
            return response
        minimum, maximum = -(2**63), 2**63-1
        keys = [minimum, *range(5000), maximum]
        sql('CREATE TABLE t(payload VARCHAR(2048),v VECTOR(2),k BIGINT PRIMARY KEY)')
        for start in range(0, len(keys), 100):
            batch = keys[start:start+100]
            sql('INSERT INTO t VALUES '+','.join("('x','[0,0]',%d)" % key for key in batch), affected_rows=len(batch))
        client.kill(); client.start()
        disk = sum(path.stat().st_size for path in Path(data).glob('t_*.dat'))
        before = read_bytes(client.p.pid)
        sql('SELECT k FROM t ORDER BY k DESC LIMIT 3', rows=[[maximum], [4999], [4998]])
        sql('SELECT k AS ident,k AS again FROM t ORDER BY ident LIMIT 2 OFFSET 1',
            columns=['ident', 'again'], rows=[[0, 0], [1, 1]])
        projection_reads = read_bytes(client.p.pid)-before
        assert projection_reads < 2*2**20, ('PK ordering read wide payloads', projection_reads, disk)
        sql('SELECT k FROM t ORDER BY k LIMIT 3 OFFSET 9223372036854775807', rows=[])
        before = read_bytes(client.p.pid)
        sql('SELECT k,payload FROM t ORDER BY payload LIMIT 3 OFFSET 9223372036854775807', rows=[])
        empty_offset_reads = read_bytes(client.p.pid)-before
        assert empty_offset_reads < 2*2**20, ('provably empty OFFSET read wide payloads', empty_offset_reads, disk)
        for invalid in (
            'SELECT absent FROM t LIMIT 1 OFFSET 9223372036854775807',
            'SELECT k FROM t ORDER BY absent LIMIT 1 OFFSET 9223372036854775807',
            "SELECT L2_DISTANCE(v,'[NaN,0]') FROM t LIMIT 1 OFFSET 9223372036854775807",
        ):
            assert client.request(invalid).get('ok') is False, invalid
        sql('BEGIN')
        sql('DELETE FROM t WHERE k=%d' % maximum, affected_rows=1)
        sql("UPDATE t SET payload='changed' WHERE k=0", affected_rows=1)
        sql('INSERT INTO t VALUES '+','.join("('pending','[0,0]',%d)" % key
            for key in range(10000, 10200)), affected_rows=200)
        assert (Path(data)/'txn.tmp').exists()
        sql('SELECT k,payload FROM t ORDER BY k LIMIT 1 OFFSET %d' % len(keys), rows=[[10001, 'pending']])
        before = read_bytes(client.p.pid)
        sql('SELECT k FROM t ORDER BY k DESC LIMIT 3', rows=[[10199], [10198], [10197]])
        transaction_reads = read_bytes(client.p.pid)-before
        assert transaction_reads < 2*2**20, ('PK projection read spilled row data', transaction_reads)
        sql('ROLLBACK')
        sql('SELECT k FROM t ORDER BY k DESC LIMIT 2', rows=[[maximum], [4999]])
        sql('BEGIN')
        sql('DELETE FROM t WHERE k=%d' % minimum, affected_rows=1)
        sql("INSERT INTO t VALUES('p','[0,0]',20000)", affected_rows=1)
        sql("UPDATE t SET payload='changed' WHERE k=0", affected_rows=1)
        sql('COMMIT')
        client.kill(); client.start()
        sql('SELECT k FROM t ORDER BY k LIMIT 3', rows=[[0], [1], [2]])
        sql('SELECT k FROM t ORDER BY k DESC LIMIT 3', rows=[[maximum], [20000], [4999]])
        # Payload projection, payload ordering and filtering still need actual rows.
        sql('SELECT k,payload FROM t ORDER BY k LIMIT 2', rows=[[0, 'changed'], [1, 'x']])
        sql('SELECT k FROM t ORDER BY payload DESC LIMIT 2', rows=[[1], [2]])
        sql("SELECT k FROM t WHERE payload='changed' ORDER BY k", rows=[[0]])
        print(json.dumps(dict(passed=True, heap_bytes=disk, projection_read_bytes=projection_reads,
            transaction_projection_read_bytes=transaction_reads, empty_offset_read_bytes=empty_offset_reads)))


if __name__ == '__main__':
    main()
