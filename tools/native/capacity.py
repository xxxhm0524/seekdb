"""Exceed the enforced native memory limit with actual persistent user data."""
import argparse
import json
import os
from pathlib import Path
import threading
import time
from native_test_support import NativeSession, ROOT, save


def run(a):
    out = ROOT / a.output; out.mkdir(exist_ok=False)
    parent = Path(json.loads((ROOT / 'native-settings.json').read_text())['cgroup'])
    group = parent / ('capacity-' + out.name); group.mkdir()
    for name, value in [('memory.max', str(2 << 30)), ('memory.swap.max', '0'), ('cpu.max', '200000 100000'), ('pids.max', '128')]: (group / name).write_text(value)
    os.environ.update(MVD_NATIVE_CGROUP=str(group), MVD_VECTOR_CACHE_MIB='0', MVD_PAGE_CACHE_MIB='1')
    samples = []; stop = threading.Event()
    def watch():
        while not stop.is_set():
            samples.append(int((group / 'memory.current').read_text())); stop.wait(.005)
    watcher = threading.Thread(target=watch); watcher.start(); db = None
    try:
        db = NativeSession(out, 'large-data', (ROOT / a.binary).resolve(strict=True))
        assert (group / 'cgroup.procs').read_text().strip() == str(db.proc.pid)
        payload = 'x' * 65536; count = 40000; start = time.monotonic()
        db.execute('CREATE TABLE t(id BIGINT PRIMARY KEY,content VARCHAR(65536),bucket BIGINT,v VECTOR(2));')
        for first in range(0, count, 10):
            rows = ','.join(f"({key},'{payload}',{key%7},'[{key%23},{key%29}]')" for key in range(first, first + 10))
            assert db.execute('INSERT INTO t VALUES ' + rows + ';')['affected_rows'] == 10
            if first % 10000 == 0: print(json.dumps(dict(rows_written=first+10)), flush=True)
        path = Path(db.temp.name) / 'database.mvd'
        size = path.stat().st_size; allocated = path.stat().st_blocks * 512
        assert count * len(payload) > 2 << 30 and size > 2 << 30 and allocated > 2 << 30
        for mode in ('exact', 'ann'):
            expected = sorted((k for k in range(count) if k % 7 == 3), key=lambda k: ((k%23)**2 + (k%29)**2, k))[:10]
            rows = db.execute("SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM t WHERE bucket=3 ORDER BY d LIMIT 10;", mode=mode)['rows']
            assert [r[0] for r in rows] == expected
        db.execute('BEGIN;'); db.execute('UPDATE t SET bucket=-1 WHERE id<100;'); db.execute('DELETE FROM t WHERE id>=100 AND id<200;')
        db.restart(crash=True)
        assert len(db.execute('SELECT id FROM t;')['rows']) == count
        assert db.execute('SELECT bucket FROM t WHERE id=0;')['rows'] == [[0]]
        db.execute('BEGIN;'); db.execute('UPDATE t SET bucket=-1 WHERE id<100;'); db.execute('DELETE FROM t WHERE id>=100 AND id<200;'); db.execute('COMMIT;')
        db.restart(crash=True)
        assert len(db.execute('SELECT id FROM t;')['rows']) == count - 100
        assert db.execute('SELECT bucket FROM t WHERE id=0;')['rows'] == [[-1]]
        assert db.execute(f'SELECT content FROM t WHERE id={count-1};')['rows'] == [[payload]]
        db.shutdown(); stop.set(); watcher.join()
        events = {k: int(v) for k, v in (line.split() for line in (group / 'memory.events').read_text().splitlines())}
        assert events['oom'] == 0 and events['oom_kill'] == 0, events
        report = dict(passed=True, rows=count, logical_text_bytes=count*len(payload), database_logical_bytes=size,
            database_allocated_bytes=allocated, memory_limit_bytes=2 << 30, sampled_cgroup_max_bytes=max(samples),
            actual_memory_peak_bytes=int((group/'memory.peak').read_text()) if (group/'memory.peak').exists() else None,
            sample_target_interval_ms=5, memory_events=events, seconds=time.monotonic()-start,
            exact_and_ann=True, committed_and_uncommitted_sigkill=True, private_cgroup=str(group),
            scope='Actual native constrained default entrypoint and >2 GiB physical user data. Sampled memory.current is a lower bound when memory.peak is unavailable. No Docker or formal score.')
        save(out/'report.json', report); print(json.dumps(report), flush=True)
    finally:
        stop.set(); watcher.join()
        if db is not None: db.close()
        group.rmdir()


if __name__ == '__main__':
    p = argparse.ArgumentParser(); p.add_argument('--output', required=True); p.add_argument('--binary', required=True); run(p.parse_args())
