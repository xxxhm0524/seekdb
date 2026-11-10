#!/usr/bin/env python3
"""Exercise >2 GiB of heap files inside a 256 MiB, read-only Docker runtime."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT/'protocol_check'))
from _transport import Session


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--image', required=True)
    parser.add_argument('--rows', type=int, default=260000)
    args = parser.parse_args()
    config = json.loads((ROOT/'protocol_check/config.json').read_text(encoding='utf-8'))
    config['memory'] = '256m'  # Stricter than the formal 2 GiB budget.
    with tempfile.TemporaryDirectory(prefix='mvd-docker-resource-') as tmp:
        session = Session(config, Path(tmp)/'stderr.log', image=args.image)
        def sql(text, **fields):
            result = session.request(text)
            assert result.get('ok') is True, (text[:120],result)
            for field,value in fields.items():
                assert result.get(field) == value, (text[:120],result)
            return result
        def device_bytes():
            return int(session.docker('exec',session.name,'stat','-c','%s','/data/t_1.dat').stdout)
        def memory_peak():
            return int(session.docker('exec',session.name,'cat','/sys/fs/cgroup/memory.peak').stdout)
        try:
            session.start()
            sql('CREATE TABLE t (id BIGINT PRIMARY KEY,s VARCHAR(2048),v VECTOR(2))')
            start_time = time.perf_counter()
            for start in range(0,args.rows,100):
                ids = range(start,min(start+100,args.rows))
                sql('INSERT INTO t VALUES '+','.join("(%d,'x','[%d,0]')" % (i,i) for i in ids),
                    affected_rows=len(ids))
            disk = device_bytes()
            assert disk > 2*2**30, ('dataset must exceed formal 2 GiB budget',disk)
            print(json.dumps(dict(stage='loaded',disk_bytes=disk,rows=args.rows,
                seconds=round(time.perf_counter()-start_time,3))),flush=True)
            session.shutdown()
            session.start()
            sql('SELECT id FROM t ORDER BY id DESC LIMIT 3',rows=[[args.rows-1],[args.rows-2],[args.rows-3]])
            sql("SELECT id FROM t ORDER BY L2_DISTANCE(v,'[0,0]') LIMIT 3 OFFSET 1",rows=[[1],[2],[3]])
            sql('BEGIN')
            sql("UPDATE t SET v='[-1,0]' WHERE id=%d" % (args.rows-1),affected_rows=1)
            sql("SELECT id FROM t ORDER BY L2_DISTANCE(v,'[0,0]') LIMIT 3",rows=[[0],[1],[args.rows-1]])
            sql('ROLLBACK')
            start_time = time.perf_counter()
            sql("UPDATE t SET s='changed'",affected_rows=args.rows)
            print(json.dumps(dict(stage='whole-table update',seconds=round(time.perf_counter()-start_time,3),
                memory_peak_bytes=memory_peak())),flush=True)
            session.crash()
            session.start()
            sql('SELECT id,s FROM t WHERE id=0',rows=[[0,'changed']])
            sql('SELECT id,s FROM t WHERE id=%d' % (args.rows-1),rows=[[args.rows-1,'changed']])
            sql('SELECT id FROM t ORDER BY id DESC LIMIT 1',rows=[[args.rows-1]])
            start_time = time.perf_counter()
            sql('DELETE FROM t',affected_rows=args.rows)
            sql('SELECT id FROM t',rows=[])
            print(json.dumps(dict(stage='whole-table delete',seconds=round(time.perf_counter()-start_time,3),
                memory_peak_bytes=memory_peak())),flush=True)
            session.shutdown()
            session.start()
            sql('SELECT id FROM t',rows=[])
            sql("INSERT INTO t VALUES (1,'after','[0,0]')",affected_rows=1)
            sql('SELECT id FROM t',rows=[[1]])
            session.shutdown()
        finally:
            session.close()
    print('Docker >2 GiB resource boundaries: PASS',flush=True)


if __name__ == '__main__':
    main()
