"""Bounded record batching must retain row refs and statement atomicity."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from client import Client


def run(directory):
    db = Client(directory, timeout=90)
    rows = {0: ['kept', [0.0, 0.0, 0.0]]}
    checked = 0
    try:
        db.execute('CREATE TABLE t(id BIGINT PRIMARY KEY,content VARCHAR(70000),v VECTOR(3));')
        db.execute("INSERT INTO t VALUES(0,'kept','[0,0,0]');")
        def value(key, text='x'):
            return f"({key},'{text}','[{key % 17},0.125,-0.25]')"
        # Cross the 1024-record metadata cap with one statement.
        values = [value(k) for k in range(1, 5001)]
        db.execute('INSERT INTO t VALUES' + ','.join(values) + ';')
        rows.update({k: ['x', [float(k % 17), 0.125, -0.25]] for k in range(1, 5001)})
        # Mix records on both sides of the 64 KiB payload bound. Large rows
        # bypass the batch and must leave correct references for adjacent rows.
        widths = [0, 1, 63, 4095, 4096, 65500, 65512, 65536, 65537, 70000]
        for base in range(6000, 6200, len(widths)):
            group = []
            for i, width in enumerate(widths):
                key = base + i; text = chr(97 + i) * width
                group.append(value(key, text)); rows[key] = [text, [float(key % 17), 0.125, -0.25]]
            db.execute('INSERT INTO t VALUES' + ','.join(group) + ';')
        def check():
            nonlocal checked
            expected = [[k, *rows[k]] for k in sorted(rows)]
            assert db.execute('SELECT * FROM t ORDER BY id;')['rows'] == expected
            checked += 1
        check()
        database = directory / 'database.mvd'
        def file_hash():
            if db.container:
                # The production container owns its mode-0600 data file. Read
                # its bytes as that same container user rather than widening
                # runtime permissions to accommodate a host-side assertion.
                return subprocess.check_output(['docker', 'exec', db.container, 'sha256sum', '/data/database.mvd'], text=True).split()[0]
            return hashlib.sha256(database.read_bytes()).hexdigest()
        before = file_hash()
        fresh = [value(k) for k in range(10000, 14500)]
        # Duplicate after several full batches; both an existing key and a
        # key introduced earlier in this very statement must undo everything.
        for tail in [value(0), value(10001), "(20000,'bad','[1,2]')"]:
            db.execute('INSERT INTO t VALUES' + ','.join(fresh + [tail]) + ';', ok=False)
            assert file_hash() == before
            check()
        db.execute('BEGIN;'); db.execute('INSERT INTO t VALUES' + value(9999) + ';')
        rows[9999] = ['x', [float(9999 % 17), 0.125, -0.25]]
        db.execute('INSERT INTO t VALUES' + ','.join(fresh + [value(9999)]) + ';', ok=False)
        check(); db.execute('COMMIT;'); db.restart(crash=True); check()
        db.execute('BEGIN;'); db.execute('INSERT INTO t VALUES' + ','.join(fresh) + ';'); db.execute('ROLLBACK;'); check()
        return dict(passed=True, final_rows=len(rows), full_state_checks=checked, failed_statement_kinds=4,
                    payload_widths=widths, late_errors_preserve_file_bytes=True, prior_transaction_statement_survives=True)
    finally:
        db.kill()


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    p = argparse.ArgumentParser(); p.add_argument('--output', required=True); args = p.parse_args()
    out = Path(args.output); out.mkdir(parents=True, exist_ok=False)
    try:
        result = run(out / 'data')
    except Exception as error:
        import traceback
        result = dict(passed=False, error=str(error), traceback=traceback.format_exc())
    (out / 'report.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result), flush=True)
    sys.exit(0 if result['passed'] else 1)
