#!/usr/bin/env python3
"""Kill across multi-table COMMIT; require all-or-nothing durable state."""
import json
import os
import queue
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(__file__))
from protocol_client import Client


def main():
    rows = 1600
    verdicts = {'old': 0, 'new': 0, 'acknowledged': 0}
    for delay in (-1, 0, 0.0002, 0.001, 0.003, 0.008, None):
        with tempfile.TemporaryDirectory(prefix='mvd-commit-') as data:
            client = Client(datadir=data, extra=['--cache-mb=1'])
            def sql(text, **fields):
                r = client.request(text)
                assert r.get('ok') is True, (text[:120], r)
                for k, v in fields.items():
                    assert r.get(k) == v, r
                return r
            try:
                sql('CREATE TABLE a (id BIGINT PRIMARY KEY,s VARCHAR(256),v VECTOR(2))')
                sql('CREATE TABLE b (id BIGINT PRIMARY KEY,s VARCHAR(256),v VECTOR(2))')
                for table in ('a', 'b'):
                    sql("INSERT INTO %s VALUES (1,'old','[9,9]'),(2,'gone','[8,8]')" % table, affected_rows=2)
                sql('BEGIN')
                for table in ('a', 'b'):
                    sql("UPDATE %s SET s='new',v='[0,0]' WHERE id=1" % table, affected_rows=1)
                    sql('DELETE FROM %s WHERE id=2' % table, affected_rows=1)
                    for start in range(3, rows+3, 100):
                        values = ','.join("(%d,'new','[%d,0]')" % (i,i) for i in range(start,start+100))
                        sql('INSERT INTO %s VALUES ' % table + values, affected_rows=100)
                answers = queue.Queue()
                ident = client.seq + 1
                reader = None
                if delay != -1:
                    reader = threading.Thread(target=lambda: answers.put(client.p.stdout.readline()), daemon=True)
                    reader.start()
                    client.p.stdin.write(json.dumps({'id':ident,'op':'execute','sql':'COMMIT','search_mode':'exact'})+'\n')
                    client.p.stdin.flush()
                    if delay is None:
                        reader.join(timeout=10)
                        assert not reader.is_alive(), 'COMMIT response timeout'
                    else:
                        time.sleep(delay)
                # Closing streams waits for the reader, so kill/wait first.
                client.p.kill()
                client.p.wait()
                if reader:
                    reader.join(timeout=5)
                acknowledged = False
                if not answers.empty():
                    line = answers.get()
                    if line:
                        response = json.loads(line)
                        assert response == {'id': ident, 'ok': True}, response
                        acknowledged = True
                client.kill()
                client = Client(datadir=data, extra=['--cache-mb=1'])
                a = sql('SELECT id,s FROM a ORDER BY id')['rows']
                b = sql('SELECT id,s FROM b ORDER BY id')['rows']
                old = [[1,'old'],[2,'gone']]
                new = [[1,'new']] + [[i,'new'] for i in range(3,rows+3)]
                assert a == b, ('cross-table partial commit', delay, len(a),len(b))
                assert a in (old,new), ('partial commit',delay,len(a),a[:4])
                assert not acknowledged or a == new, ('acknowledged commit lost',delay)
                state = 'new' if a == new else 'old'
                verdicts[state] += 1
                verdicts['acknowledged'] += acknowledged
                sql("INSERT INTO a VALUES (99999,'after','[0,0]')", affected_rows=1)
                print(json.dumps(dict(delay=delay, acknowledged=acknowledged, recovered=state)), flush=True)
            finally:
                client.close()
    assert verdicts['old'] and verdicts['new'], verdicts
    assert verdicts['acknowledged'], verdicts
    print('commit crash boundaries: PASS', json.dumps(verdicts))


if __name__ == '__main__':
    main()
