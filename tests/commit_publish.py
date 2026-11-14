"""Kill the production ELF at actual root-publication/sync syscall boundaries."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import threading
import time
from client import Client, ROOT


def run(output, shim):
    assert os.name == 'posix' and not os.environ.get('MVD_DOCKER_IMAGE')
    observations = []
    for stage in ['before_root', 'after_root', 'before_sync', 'after_sync']:
        directory = output / stage
        db = Client(directory, timeout=30)
        try:
            db.execute('CREATE TABLE a(id BIGINT PRIMARY KEY,g BIGINT,v VECTOR(3));')
            db.execute('CREATE TABLE b(id BIGINT PRIMARY KEY,g BIGINT);')
            db.execute("INSERT INTO a VALUES(0,0,'[0,0,0]');")
            db.execute('INSERT INTO b VALUES(0,0);'); db.shutdown()
            arm, marker = directory / 'commit.arm', directory / 'commit.stopped'
            db.env.update(LD_PRELOAD=str(shim), MVD_COMMIT_STOP=stage, MVD_COMMIT_MARKER=str(marker), MVD_COMMIT_ARM=str(arm))
            db.start(); db.execute('BEGIN;')
            db.execute("UPDATE a SET g=1,v='[1,2,3]' WHERE id=0;")
            db.execute('UPDATE b SET g=1 WHERE id=0;')
            db.execute('INSERT INTO b VALUES' + ','.join(f'({k},1)' for k in range(1, 1001)) + ';')
            arm.write_text('armed')
            outcome = {}
            def commit():
                try:
                    outcome['response'] = db.execute('COMMIT;')
                except Exception:
                    outcome['interrupted'] = True
            worker = threading.Thread(target=commit, daemon=True); worker.start()
            deadline = time.monotonic() + 15
            while not marker.exists() and time.monotonic() < deadline:
                assert db.process.poll() is None, 'database exited before the selected syscall'
                time.sleep(.005)
            assert marker.exists(), 'precise syscall cut was not reached'
            assert marker.read_text() == stage
            db.kill(); worker.join(timeout=10); assert not worker.is_alive()
            for name in ['LD_PRELOAD', 'MVD_COMMIT_STOP', 'MVD_COMMIT_MARKER', 'MVD_COMMIT_ARM']:
                db.env.pop(name, None)
            db.start()
            expected = 0 if stage == 'before_root' else 1
            assert db.execute('SELECT g,v FROM a;')['rows'] == [[expected, [0.0, 0.0, 0.0] if not expected else [1.0, 2.0, 3.0]]]
            assert db.execute('SELECT id,g FROM b ORDER BY id;')['rows'] == [[k, expected] for k in range(1001 if expected else 1)]
            observations.append(dict(stage=stage, recovered_generation=expected, tables_atomic=True, acknowledged=bool(outcome.get('response', {}).get('ok'))))
        finally:
            db.kill()
    # Count actual durability barriers in an ordinary acknowledged transaction.
    directory = output / 'sync-count'; report = directory / 'io-counts.json'
    db = Client(directory, env={'LD_PRELOAD': str(shim), 'MVD_IO_REPORT': str(report)})
    try:
        db.execute('CREATE TABLE t(id BIGINT PRIMARY KEY);'); db.shutdown()
        db.start(); db.execute('BEGIN;')
        db.execute('INSERT INTO t VALUES' + ','.join(f'({k})' for k in range(5000)) + ';')
        db.execute('COMMIT;'); db.shutdown()
        counts = json.loads(report.read_text())
        assert counts['database_sync_calls'] == 1, counts
        assert counts['database_write_calls'] < 300, counts
        db.start(); assert db.execute('SELECT id FROM t;')['rows'] == [[k] for k in range(5000)]
    finally:
        db.kill()
    return dict(passed=True, precise_commit_interruptions=observations, acknowledged_sync_count=counts['database_sync_calls'],
                batched_5000_rows_write_calls=counts['database_write_calls'], process_crash_model=True,
                limitation='Tests terminate a process in a surviving kernel; they do not establish machine-power-loss ordering.')


if __name__ == '__main__':
    p=argparse.ArgumentParser();p.add_argument('--output',required=True);args=p.parse_args()
    out=Path(args.output).resolve();out.mkdir(parents=True,exist_ok=False)
    shim=out/'io-probe.so';subprocess.run(['cc','-shared','-fPIC','-O2',str(ROOT/'tests/io_probe.c'),'-o',str(shim),'-ldl'],check=True)
    try:result=run(out,shim)
    except Exception as error:
        import traceback
        result=dict(passed=False,error=str(error),traceback=traceback.format_exc())
    (out/'report.json').write_text(json.dumps(result,indent=2),encoding='utf-8');print(json.dumps(result),flush=True)
    raise SystemExit(0 if result['passed'] else 1)
