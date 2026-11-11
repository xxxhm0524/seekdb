#!/usr/bin/env python3
"""Check wide-row bytes and recovery when zero blocks replace nonzero data."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'protocol_check'))
from _transport import Session


def main():
    parser = argparse.ArgumentParser()
    target = parser.add_mutually_exclusive_group()
    target.add_argument('--image')
    target.add_argument('--binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--expect-sparse', action='store_true')
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    config = json.loads((ROOT / 'protocol_check/config.json').read_text(encoding='utf-8'))
    config['memory'] = '256m'
    binary = args.binary or ROOT / ('minivectordb.exe' if os.name == 'nt' else 'minivectordb')
    session = Session(config, args.output.with_suffix('.stderr.log'), image=args.image,
                      command=None if args.image else [str(binary.resolve()), '--data-dir', '{data_dir}', '--cache-mb=1'])
    snapshots = []

    def sql(text, **expected):
        response = session.request(text)
        assert response.get('ok') is True, (text[:120], response)
        for key, value in expected.items():
            assert response.get(key) == value, (key, response, value)
        return response

    def snapshot(stage):
        if args.image:
            raw = session.docker('exec', session.name, 'cat', '/data/t_1.dat').stdout
            blocks, unit = map(int, session.docker('exec', session.name, 'stat', '-c', '%b %B', '/data/t_1.dat').stdout.split())
            allocation = blocks * unit
        else:
            path = Path(session.temp.name) / 't_1.dat'
            raw = path.read_bytes()
            allocation = getattr(path.stat(), 'st_blocks', None)
            if allocation is not None:
                allocation *= 512
        result = dict(stage=stage, logical_bytes=len(raw), allocated_bytes=allocation,
                      sha256=hashlib.sha256(raw).hexdigest())
        snapshots.append(result)
        return result

    count = 4099  # Full pages, a partial last page, and multiple I/O batches.
    dense = '\U0001f600' * 2048
    expected = {i: dense for i in range(count)}
    try:
        session.start()
        sql('CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(2048),v VECTOR(2))')
        for start in range(0, count, 100):
            stop = min(start + 100, count)
            sql('INSERT INTO t VALUES ' + ','.join("(%d,'%s','[%d,0]')" % (i, dense, i % 17)
                for i in range(start, stop)), affected_rows=stop-start)
        session.shutdown(); session.start()
        before = snapshot('dense table after restart')
        # Clear old nonzero blocks, including the buffered read-ahead path.
        sql("UPDATE t SET s='x'", affected_rows=count)
        expected = {i: 'x' for i in expected}
        session.shutdown(); session.start()
        after = snapshot('short text after restart')
        assert before['logical_bytes'] == after['logical_bytes']
        if args.expect_sparse and after['allocated_bytes'] is not None:
            assert after['allocated_bytes'] < after['logical_bytes'] * .70, snapshots
        sql('BEGIN')
        sql("UPDATE t SET s='%s' WHERE id>=2000 AND id<=2010" % dense, affected_rows=11)
        sql('DELETE FROM t WHERE id>=3000', affected_rows=count-3000)
        sql('ROLLBACK')
        # Mix holes, surviving wide values, an empty string and a reinserted key.
        sql("UPDATE t SET s='%s' WHERE id>=2000 AND id<=2010" % dense, affected_rows=11)
        for i in range(2000, 2011):
            expected[i] = dense
        sql('DELETE FROM t WHERE id>=3000', affected_rows=count-3000)
        expected = {i: value for i, value in expected.items() if i < 3000}
        sql("INSERT INTO t VALUES (3500,'','[0,0]')", affected_rows=1)
        expected[3500] = ''
        session.crash(); session.start()
        for start in range(0, count, 40):
            rows = sql('SELECT id,s FROM t WHERE id>=%d AND id<%d ORDER BY id' % (start, start+40))['rows']
            assert rows == [[i, expected[i]] for i in range(start, min(start+40, count)) if i in expected]
        sql('DELETE FROM t', affected_rows=len(expected))
        session.crash(); session.start()
        sql('SELECT id FROM t', rows=[])
        sql("INSERT INTO t VALUES (99,'%s','[3,4]')" % dense, affected_rows=1)
        session.shutdown(); session.start()
        sql('SELECT id,s,v FROM t', rows=[[99, dense, [3, 4]]])
        snapshot('deleted slots followed by a new wide row')
        session.shutdown()
        report = dict(passed=True, snapshots=snapshots, old_nonzero_blocks_cleared=True,
                      rollback_and_crash_recovery=True, request_timeout=config['request_timeout'])
        args.output.write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
        print(json.dumps(report), flush=True)
    finally:
        session.close()


if __name__ == '__main__':
    main()
