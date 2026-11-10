#!/usr/bin/env python3
"""SIGKILL recovery, including repeated kills while recovery is in progress."""
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from protocol_client import Client, DEFAULT_BINARY  # noqa: E402

ROWS = 50000
DIM = 32
BATCH = 500


def main():
    rows = ROWS
    if len(sys.argv) > 1:
        rows = int(sys.argv[1])
    with tempfile.TemporaryDirectory(prefix="mvd-crash-", ignore_cleanup_errors=True) as datadir:
        client = Client(DEFAULT_BINARY, datadir, extra=["--cache-mb=1"])
        client.request("CREATE TABLE t (id BIGINT PRIMARY KEY, cat VARCHAR(8), v VECTOR(%d));" % DIM)
        for start in range(0, rows, BATCH):
            parts = []
            for i in range(start, min(start + BATCH, rows)):
                values = ",".join("%.7g" % (((i * 13 + j * 7) % 101) / 100.0) for j in range(DIM))
                parts.append("(%d,'c%d','[%s]')" % (i + 1, i % 10, values))
            response = client.request("INSERT INTO t VALUES " + ",".join(parts) + ";")
            if response.get("affected_rows") != len(parts):
                raise SystemExit("insert failed: %r" % (response,))
        # Leave an uncommitted transaction that must disappear after the kill.
        client.request("BEGIN;")
        client.request("INSERT INTO t VALUES (%d,'x','[0,0]');" % (rows + 1))
        client.kill()

        # Repeatedly kill the process at short delays, possibly during WAL replay.
        for delay in (0.01, 0.03, 0.08):
            proc = subprocess.Popen(
                [DEFAULT_BINARY, "--data-dir=" + datadir, "--cache-mb=1"],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            time.sleep(delay)
            proc.kill()
            proc.wait()

        client = Client(DEFAULT_BINARY, datadir, extra=["--cache-mb=1"])
        response = client.request("SELECT id FROM t ORDER BY id;")
        ids = [row[0] for row in response.get("rows", [])]
        ok = len(ids) == rows and ids[0] == 1 and ids[-1] == rows
        if not ok:
            print("FAIL: recovered %d rows, first=%r last=%r" %
                  (len(ids), ids[0] if ids else None, ids[-1] if ids else None))
            return 1
        response = client.request(
            "SELECT id, L2_DISTANCE(v,'[%s]') AS d FROM t WHERE id = %d;" % (",".join(["0"] * DIM), rows))
        if not response.get("rows") or response["rows"][0][0] != rows:
            print("FAIL: vector row missing after recovery:", response)
            return 1
        client.request("INSERT INTO t VALUES (%d,'after','[0,0]');" % (rows + 2))
        client.shutdown()
        print("crash recovery: PASS (%d rows, repeated kills during recovery)" % rows)
    return 0


if __name__ == "__main__":
    sys.exit(main())
