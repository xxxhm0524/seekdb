#!/usr/bin/env python3
"""SIGKILL recovery, including repeated kills while recovery is in progress."""
import os
import argparse
import queue
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from protocol_client import Client, DEFAULT_BINARY  # noqa: E402

ROWS = 50000
DIM = 32
BATCH = 500


def main():
    parser=argparse.ArgumentParser();parser.add_argument('rows',nargs='?',type=int,default=ROWS);parser.add_argument('--wide-rows',action='store_true');args=parser.parse_args()
    rows = args.rows
    with tempfile.TemporaryDirectory(prefix="mvd-crash-", ignore_cleanup_errors=True) as datadir:
        client = Client(DEFAULT_BINARY, datadir, extra=["--cache-mb=1"])
        def sql(text, **expected):
            response = client.request(text)
            assert response.get('ok') is True, (text[:120], response)
            for field, value in expected.items():
                assert response.get(field) == value, response
            return response
        sql("CREATE TABLE t (id BIGINT PRIMARY KEY, cat VARCHAR(%d), v VECTOR(%d));" % (25000 if args.wide_rows else 8,DIM))
        for start in range(0, rows, BATCH):
            parts = []
            for i in range(start, min(start + BATCH, rows)):
                values = ",".join("%.7g" % (((i * 13 + j * 7) % 101) / 100.0) for j in range(DIM))
                parts.append("(%d,'c%d','[%s]')" % (i + 1, i % 10, values))
            response = sql("INSERT INTO t VALUES " + ",".join(parts) + ";")
            if response.get("affected_rows") != len(parts):
                raise SystemExit("insert failed: %r" % (response,))
        # Leave an uncommitted transaction that must disappear after the kill.
        zero = '[' + ','.join(['0'] * DIM) + ']'
        sql("BEGIN;")
        sql("INSERT INTO t VALUES (%d,'x','%s');" % (rows + 1, zero), affected_rows=1)
        sql("UPDATE t SET cat='dirty',v='%s' WHERE id=1;" % zero, affected_rows=1)
        sql("DELETE FROM t WHERE id=%d;" % rows, affected_rows=1)
        sql("SELECT id,cat FROM t WHERE id=1;", rows=[[1,'dirty']])
        client.kill()

        # Observe writes before ready to distinguish an actual recovery kill
        # from killing a process that has already finished recovery.
        observed_recovery_kills = 0
        for delay in (0.01, 0.03, 0.08):
            table_file = os.path.join(datadir, 't_1.dat')
            before = os.stat(table_file).st_mtime_ns
            ready = queue.Queue()
            proc = subprocess.Popen(
                [DEFAULT_BINARY, "--data-dir=" + datadir, "--cache-mb=1"],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            reader = threading.Thread(target=lambda: ready.put(proc.stdout.readline()), daemon=True)
            reader.start()
            deadline = time.monotonic() + 10
            wrote = False
            while ready.empty() and time.monotonic() < deadline:
                if os.stat(table_file).st_mtime_ns != before:
                    wrote = True
                    break
                time.sleep(0.001)
            # First attempt kills as soon as WAL replay writes are observed.
            # Later attempts also exercise later recovery/ready boundaries.
            if delay != 0.01:
                time.sleep(delay)
            if wrote and ready.empty():
                observed_recovery_kills += 1
            proc.kill()
            proc.wait()
            reader.join(timeout=5)
            proc.stdin.close()
            proc.stdout.close()

        client = Client(DEFAULT_BINARY, datadir, extra=["--cache-mb=1"])
        response = sql("SELECT id FROM t ORDER BY id;")
        ids = [row[0] for row in response.get("rows", [])]
        ok = ids == list(range(1, rows + 1))
        if not ok:
            print("FAIL: recovered %d rows, first=%r last=%r" %
                  (len(ids), ids[0] if ids else None, ids[-1] if ids else None))
            return 1
        sql("SELECT cat FROM t WHERE id=1;", rows=[['c0']])
        response = sql(
            "SELECT id, L2_DISTANCE(v,'[%s]') AS d FROM t WHERE id = %d;" % (",".join(["0"] * DIM), rows))
        if not response.get("rows") or response["rows"][0][0] != rows:
            print("FAIL: vector row missing after recovery:", response)
            return 1
        sql("INSERT INTO t VALUES (%d,'after','%s');" % (rows + 2, zero), affected_rows=1)
        client.shutdown()
        assert observed_recovery_kills > 0, 'no kill was observed between recovery writes and ready'
        print("crash recovery: PASS (%d rows, %d verified kill(s) after recovery writes and before ready)" %
              (rows, observed_recovery_kills))
    return 0


if __name__ == "__main__":
    sys.exit(main())
