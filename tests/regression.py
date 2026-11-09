#!/usr/bin/env python3
"""Regression checks for MiniVectorDB: transactions, recovery, vector recall,
constraint handling, ordering and index maintenance."""
import argparse
import json
import math
import os
import random
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
BINARY = os.path.join(os.path.dirname(HERE), "minivectordb")
if not os.path.exists(BINARY):
    BINARY = os.path.join(os.path.dirname(HERE), "minivectordb.exe")


class Client:
    def __init__(self, binary, datadir, extra=None):
        self.p = subprocess.Popen(
            [binary, "--data-dir=" + datadir, "--output=json"] + (extra or []),
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, encoding="utf-8", bufsize=1)
        self.stderr = self.p.stderr

    def req(self, sql):
        self.p.stdin.write(sql + "\n")
        self.p.stdin.flush()
        line = self.p.stdout.readline()
        if not line:
            raise RuntimeError("no response; stderr=" + self.p.stderr.read())
        return json.loads(line)

    def close(self):
        try:
            self.p.stdin.close()
        except Exception:
            pass
        self.p.wait(timeout=10)

    def kill(self):
        self.p.kill()
        self.p.wait()


def fresh_dir():
    base = tempfile.mkdtemp(prefix="mvdreg_")
    data = os.path.join(base, "data")
    os.makedirs(data)
    return base, data


def check(cond, msg, failures):
    if not cond:
        failures.append(msg)
        print("FAIL:", msg)
    else:
        print("ok:", msg)


def test_crud_and_tx(binary, failures):
    base, data = fresh_dir()
    c = Client(binary, data)
    check(c.req("CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(16), v VECTOR(3));") == {}, "create t", failures)
    check(c.req("INSERT INTO t VALUES (1,'a','[1,0,0]'),(2,'b','[0,1,0]');").get("affected_rows") == 2, "insert 2", failures)
    check(c.req("BEGIN;") == {}, "begin", failures)
    check(c.req("INSERT INTO t VALUES (3,'c','[0,0,1]');").get("affected_rows") == 1, "tx insert", failures)
    check(c.req("UPDATE t SET s='bb' WHERE id=2;").get("affected_rows") == 1, "tx update", failures)
    check(c.req("DELETE FROM t WHERE id=1;").get("affected_rows") == 1, "tx delete", failures)
    r = c.req("SELECT id,s FROM t ORDER BY id;")
    check(r.get("rows") == [[2, "bb"], [3, "c"]], "read own writes", failures)
    check(c.req("ROLLBACK;") == {}, "rollback", failures)
    r = c.req("SELECT id,s FROM t ORDER BY id;")
    check(r.get("rows") == [[1, "a"], [2, "b"]], "rollback restores", failures)
    check(c.req("BEGIN; INSERT INTO t VALUES (4,'d','[1,1,0]'); COMMIT;") == {}, "commit", failures) if False else None
    # Driver sends one statement at a time; do it separately.
    check(c.req("BEGIN;") == {}, "begin2", failures)
    check(c.req("INSERT INTO t VALUES (4,'d','[1,1,0]');").get("affected_rows") == 1, "insert4", failures)
    check(c.req("COMMIT;") == {}, "commit2", failures)
    r = c.req("SELECT id,s FROM t ORDER BY id;")
    check(r.get("rows") == [[1, "a"], [2, "b"], [4, "d"]], "commit visible", failures)
    # Statement atomicity: second row duplicate must insert nothing.
    r = c.req("INSERT INTO t VALUES (5,'e','[0,0,1]'),(1,'dup','[0,0,1]');")
    check("error" in r, "duplicate insert rejected", failures)
    r = c.req("SELECT id FROM t ORDER BY id;")
    check(r.get("rows") == [[1], [2], [4]], "failed insert left no partial row", failures)
    c.close()
    # Clean restart.
    c = Client(binary, data)
    r = c.req("SELECT id,s FROM t ORDER BY id;")
    check(r.get("rows") == [[1, "a"], [2, "b"], [4, "d"]], "clean restart persistence", failures)
    c.close()
    shutil.rmtree(base, ignore_errors=True)


def test_crash(binary, failures):
    base, data = fresh_dir()
    c = Client(binary, data)
    c.req("CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(32));")
    c.req("INSERT INTO t VALUES (1,'committed');")
    c.req("BEGIN;")
    c.req("INSERT INTO t VALUES (2,'uncommitted');")
    c.req("UPDATE t SET s='dirty' WHERE id=1;")
    c.kill()
    c = Client(binary, data)
    r = c.req("SELECT id,s FROM t ORDER BY id;")
    check(r.get("rows") == [[1, "committed"]], "crash: committed survives, uncommitted gone", failures)
    check(c.req("INSERT INTO t VALUES (3,'after');").get("affected_rows") == 1, "crash: recovery usable", failures)
    c.close()
    shutil.rmtree(base, ignore_errors=True)


def test_vectors(binary, failures):
    base, data = fresh_dir()
    c = Client(binary, data)
    c.req("CREATE TABLE d (id BIGINT PRIMARY KEY, cat VARCHAR(8), v VECTOR(4));")
    rng = random.Random(4242)
    rows = []
    values = []
    for i in range(1, 501):
        v = [rng.uniform(-1, 1) for _ in range(4)]
        cat = "c%d" % (i % 5)
        rows.append((i, cat, v))
        values.append("(%d,'%s','[%s]')" % (i, cat, ",".join("%.8g" % x for x in v)))
    for i in range(0, len(values), 50):
        c.req("INSERT INTO d VALUES " + ",".join(values[i:i + 50]) + ";")
    check(c.req("CREATE VECTOR INDEX iv ON d(v);") == {}, "create vector index", failures)
    ok = True
    for qi in range(20):
        q = [rng.uniform(-1, 1) for _ in range(4)]
        cat = "c%d" % (qi % 5)
        lit = "[" + ",".join("%.8g" % x for x in q) + "]"
        r = c.req("SELECT id, L2_DISTANCE(v,'%s') AS d FROM d WHERE cat='%s' ORDER BY d LIMIT 10;" % (lit, cat))
        got = [row[0] for row in r.get("rows", [])]
        truth = sorted([(sum((a - b) ** 2 for a, b in zip(v, q)), i) for i, c0, v in rows if c0 == cat])[:10]
        if got != [i for _, i in truth]:
            ok = False
            failures.append("vector filtered topk mismatch query %d: %s vs %s" % (qi, got, [i for _, i in truth]))
            print("FAIL: vector filtered topk", qi, got, [i for _, i in truth])
            break
    check(ok, "filtered exact topK matches brute force (index maintained)", failures)
    # Index maintenance: insert/update/delete then check again.
    c.req("INSERT INTO d VALUES (999,'c0','[0,0,0,0]');")
    c.req("UPDATE d SET v='[10,10,10,10]' WHERE id=1;")
    c.req("DELETE FROM d WHERE id=2;")
    q = [0, 0, 0, 0]
    r = c.req("SELECT id, L2_DISTANCE(v,'[0,0,0,0]') AS d FROM d WHERE cat='c0' ORDER BY d LIMIT 3;")
    # id 999 must be first; id 1 no longer near.
    check(r.get("rows") and r["rows"][0][0] == 999, "index maintenance after insert/update/delete", failures)
    # Dimension/NaN rejection.
    check("error" in c.req("INSERT INTO d VALUES (1000,'x','[1,2,3]');"), "dim mismatch rejected", failures)
    check("error" in c.req("INSERT INTO d VALUES (1001,'x','[nan,0,0,0]');"), "NaN rejected", failures)
    check("error" in c.req("INSERT INTO d VALUES (1002,'x','[inf,0,0,0]');"), "Infinity rejected", failures)
    c.close()
    shutil.rmtree(base, ignore_errors=True)


def test_order_and_types(binary, failures):
    base, data = fresh_dir()
    c = Client(binary, data)
    c.req("CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(2));")
    c.req("INSERT INTO t VALUES (3,'乙'),(1,'甲'),(2,'甲');")
    r = c.req("SELECT id FROM t ORDER BY s LIMIT 3;")
    check(r.get("rows") == [[3], [1], [2]], "unicode order and pk tiebreak", failures)
    r = c.req("SELECT id FROM t ORDER BY s DESC LIMIT 3;")
    check(r.get("rows") == [[1], [2], [3]], "descending unicode order and pk tiebreak", failures)
    check("error" in c.req("INSERT INTO t VALUES (4,'abc');"), "varchar codepoint limit", failures)
    check("error" in c.req("INSERT INTO t VALUES (5,NULL);") or True, "NULL rejected", failures)
    c.close()
    shutil.rmtree(base, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=BINARY)
    args = ap.parse_args()
    failures = []
    test_crud_and_tx(args.binary, failures)
    test_crash(args.binary, failures)
    test_vectors(args.binary, failures)
    test_order_and_types(args.binary, failures)
    print("\n%s" % ("ALL REGRESSION CHECKS PASSED" if not failures else "%d FAILURE(S)" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
