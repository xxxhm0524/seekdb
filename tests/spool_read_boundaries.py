#!/usr/bin/env python3
"""A spilled multi-table commit must not amplify random small-row reads."""
from contextlib import ExitStack
import json
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from protocol_client import Client
from mutation_io import read_bytes


def main():
    with tempfile.TemporaryDirectory(prefix='mvd-spool-read-') as data, ExitStack() as cleanup:
        client = Client(datadir=data, extra=['--cache-mb=1'])
        cleanup.callback(client.close)
        def sql(text, **fields):
            response = client.request(text)
            assert response.get('ok') is True, (text[:100], response)
            for key, value in fields.items():
                assert response.get(key) == value, (text[:100], response)
            return response
        for table in ('a', 'b'):
            sql('CREATE TABLE %s(id BIGINT PRIMARY KEY,s VARCHAR(256),v VECTOR(2))' % table)
            sql("INSERT INTO %s VALUES (1,'old','[9,9]'),(2,'gone','[8,8]')" % table, affected_rows=2)
        sql('BEGIN')
        for table in ('a', 'b'):
            sql("UPDATE %s SET s='new',v='[0,0]' WHERE id=1" % table, affected_rows=1)
            sql('DELETE FROM %s WHERE id=2' % table, affected_rows=1)
            for start in range(3, 5003, 100):
                sql('INSERT INTO %s VALUES ' % table + ','.join("(%d,'new','[%d,0]')" % (i, i)
                    for i in range(start, start+100)), affected_rows=100)
        payload = (Path(data) / 'txn.tmp').stat().st_size
        assert payload > 8*2**20
        before = read_bytes(client.p.pid)
        sql('COMMIT')
        actual = read_bytes(client.p.pid)-before
        assert actual < payload*4+4*2**20, ('spool read amplification', actual, payload)
        client.kill(); client.start()
        for table in ('a', 'b'):
            sql('SELECT id,s,v FROM %s WHERE id<=2' % table, rows=[[1, 'new', [0, 0]]])
            sql('SELECT id FROM %s ORDER BY id DESC LIMIT 2' % table, rows=[[5002], [5001]])
        print(json.dumps(dict(passed=True, spool_bytes=payload, commit_read_bytes=actual)))


if __name__ == '__main__':
    main()
