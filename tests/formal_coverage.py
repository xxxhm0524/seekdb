#!/usr/bin/env python3
"""Coverage mapped to the formal MiniVectorDB requirements.

Each section corresponds to a row in the task statement: schema, SQL, exact
TopK, transactions, persistence/recovery, index maintenance, resource limits.
"""
import math
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from protocol_client import Client, DEFAULT_BINARY  # noqa: E402

FAILURES = []


def check(name, condition, detail=""):
    print(("PASS " if condition else "FAIL ") + name + ((" :: " + detail) if not condition else ""))
    if not condition:
        FAILURES.append(name + ((" :: " + detail) if detail else ""))


def ok(client, sql, **fields):
    r = client.request(sql)
    good = r.get("ok") is True
    for k, v in fields.items():
        good = good and r.get(k) == v
    return good, r


def err(client, sql):
    r = client.request(sql)
    code = r.get("error", {}).get("code") if isinstance(r.get("error"), dict) else None
    return r.get("ok") is False and isinstance(code, str) and bool(code), r


def section_schema(binary, d):
    c = Client(binary, d)
    cases = [
        ("CREATE TABLE bad1 (id BIGINT, v VARCHAR(4));", "missing primary key"),
        ("CREATE TABLE bad2 (id VARCHAR(4) PRIMARY KEY);", "varchar primary key"),
        ("CREATE TABLE bad3 (id BIGINT PRIMARY KEY, a VECTOR(2), b VECTOR(2));", "two vector columns"),
        ("CREATE TABLE bad4 (id BIGINT PRIMARY KEY, id VARCHAR(4));", "duplicate column"),
        ("CREATE TABLE bad5 (id BIGINT PRIMARY KEY, v VECTOR(0));", "zero vector dimension"),
        ("CREATE TABLE bad6 (id BIGINT PRIMARY KEY, s VARCHAR(0));", "zero varchar length"),
    ]
    for sql, name in cases:
        good, r = err(c, sql)
        check("schema rejects " + name, good, str(r))
    good, r = ok(c, "CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), content VARCHAR(16), embedding VECTOR(2));")
    check("schema accepts valid table", good, str(r))
    good, r = ok(c, "CREATE TABLE IF NOT EXISTS docs (id BIGINT PRIMARY KEY);")
    check("CREATE TABLE IF NOT EXISTS is no-op", good, str(r))
    good, r = ok(c, "DROP TABLE IF EXISTS never_created;")
    check("DROP TABLE IF EXISTS missing is no-op", good, str(r))
    c.shutdown()


def section_sql(binary, d):
    c = Client(binary, d)
    ok(c, "CREATE TABLE t (id BIGINT PRIMARY KEY, cat VARCHAR(8), s VARCHAR(16), v VECTOR(2));")
    good, r = ok(c, "INSERT INTO t (cat, id, s, v) VALUES ('b', 5, 'e', '[1,0]'), ('a', 4, 'd', '[0,1]'), ('b', 3, 'c', '[-1,0]'), ('a', 2, 'b', '[0,-1]'), ('c', 1, 'a', '[2,0]');",
                 affected_rows=5)
    check("multi-row INSERT with column list", good, str(r))
    good, r = ok(c, "SELECT id FROM t WHERE id >= 2 AND id < 5 ORDER BY id;")
    check("AND range predicate", good and r["rows"] == [[2], [3], [4]], str(r))
    for op, expected in [
        ("id = 3", [3]), ("id != 3", [1, 2, 4, 5]), ("id <> 3", [1, 2, 4, 5]),
        ("id < 3", [1, 2]), ("id <= 3", [1, 2, 3]), ("id > 3", [4, 5]), ("id >= 3", [3, 4, 5]),
    ]:
        good, r = ok(c, "SELECT id FROM t WHERE %s ORDER BY id;" % op)
        check("comparison " + op, good and r["rows"] == [[x] for x in expected], str(r))
    good, r = ok(c, "SELECT id FROM t WHERE cat = 'b' ORDER BY s DESC, id DESC;")
    check("multi-key ORDER BY DESC", good and r["rows"] == [[5], [3]], str(r))
    good, r = ok(c, "SELECT id FROM t ORDER BY id LIMIT 2 OFFSET 1;")
    check("LIMIT OFFSET", good and r["rows"] == [[2], [3]], str(r))
    good, r = ok(c, "SELECT id FROM t ORDER BY id LIMIT 0;")
    check("LIMIT 0", good and r["rows"] == [], str(r))
    good, r = ok(c, "SELECT * FROM t WHERE id = 1;")
    check("SELECT * column order and values",
          good and r["columns"] == ["id", "cat", "s", "v"] and r["rows"] == [[1, "c", "a", [2.0, 0.0]]],
          str(r))
    good, r = ok(c, "UPDATE t SET s = 'zz', v = '[3,4]' WHERE id <= 2;", affected_rows=2)
    check("multi-column UPDATE range", good, str(r))
    good, r = ok(c, "SELECT s, v FROM t WHERE id = 2;")
    check("UPDATE values visible", good and r["rows"] == [["zz", [3.0, 4.0]]], str(r))
    good, r = ok(c, "DELETE FROM t WHERE id >= 4;", affected_rows=2)
    check("DELETE range affected", good, str(r))
    good, r = ok(c, "SELECT id FROM t ORDER BY id;")
    check("DELETE range visible", good and r["rows"] == [[1], [2], [3]], str(r))
    good, r = ok(c, "UPDATE t SET s = 'x' WHERE id = 999;", affected_rows=0)
    check("UPDATE no match affected 0", good, str(r))
    good, r = ok(c, "DELETE FROM t WHERE id = 999;", affected_rows=0)
    check("DELETE no match affected 0", good, str(r))
    c.shutdown()


def section_vector(binary, d):
    c = Client(binary, d)
    ok(c, "CREATE TABLE v (id BIGINT PRIMARY KEY, cat VARCHAR(4), emb VECTOR(2));")
    ok(c, "INSERT INTO v VALUES (5,'b','[1,0]'),(4,'a','[0,1]'),(3,'b','[-1,0]'),(2,'a','[0,-1]'),(1,'c','[2,0]');")
    good, r = ok(c, "SELECT id FROM v ORDER BY L2_DISTANCE(emb, '[0,0]') LIMIT 3;")
    check("TopK without projected distance", good and r["rows"] == [[2], [3], [4]], str(r))
    good, r = ok(c, "SELECT id, L2_DISTANCE(emb, '[2,0]') AS d FROM v WHERE cat = 'a' ORDER BY d LIMIT 5;")
    check("filter applied before TopK", good and [row[0] for row in r["rows"]] == [2, 4], str(r))
    good, r = ok(c, "SELECT id FROM v WHERE cat = 'zzz' ORDER BY L2_DISTANCE(emb, '[0,0]') LIMIT 3;")
    check("empty filtered TopK", good and r["rows"] == [], str(r))
    good, r = ok(c, "SELECT id, L2_DISTANCE(emb, '[0,0]') AS d FROM v ORDER BY d DESC LIMIT 2;")
    check("distance DESC ordering", good and [row[0] for row in r["rows"]] == [1, 2], str(r))
    good, r = ok(c, "SELECT id, L2_DISTANCE(emb, '[3,4]') AS d FROM v WHERE id = 1;")
    check("euclidean distance value", good and abs(r["rows"][0][1] - math.sqrt(17)) < 1e-5, str(r))
    good, r = ok(c, "INSERT INTO v VALUES (6,'x','[16777217,0]'),(7,'x','[16777216,0]');", affected_rows=2)
    good2, r2 = ok(c, "SELECT id, L2_DISTANCE(emb, '[16777216,0]') AS d FROM v WHERE id >= 6 ORDER BY id;")
    check("float32 storage semantics", good and good2 and r2["rows"] == [[6, 0.0], [7, 0.0]], str(r2))
    for sql, name in [
        ("INSERT INTO v VALUES (8,'x','[NaN,0]');", "NaN"),
        ("INSERT INTO v VALUES (8,'x','[Infinity,0]');", "Infinity"),
        ("INSERT INTO v VALUES (8,'x','[-Infinity,0]');", "-Infinity"),
        ("INSERT INTO v VALUES (8,'x','[1]');", "dimension mismatch"),
        ("INSERT INTO v VALUES (8,'x','[1e100,0]');", "float32 overflow"),
        ("UPDATE v SET emb = '[1,2,3]' WHERE id = 1;", "UPDATE dimension mismatch"),
        ("SELECT id FROM v ORDER BY L2_DISTANCE(emb, '[1,2,3]') LIMIT 1;", "query dimension mismatch"),
    ]:
        good, r = err(c, sql)
        check("reject " + name, good, str(r))
    good, r = ok(c, "SELECT emb FROM v WHERE id = 1;")
    check("vector projection is JSON array", good and r["rows"] == [[[2.0, 0.0]]], str(r))
    c.shutdown()


def section_transactions(binary, d):
    c = Client(binary, d)
    ok(c, "CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(8), v VECTOR(2));")
    ok(c, "INSERT INTO t VALUES (1,'a','[0,0]'),(2,'a','[1,0]');")
    ok(c, "BEGIN;")
    ok(c, "INSERT INTO t VALUES (3,'b','[0,1]');", affected_rows=1)
    ok(c, "UPDATE t SET s = 'x' WHERE id = 1;", affected_rows=1)
    ok(c, "DELETE FROM t WHERE id = 2;", affected_rows=1)
    good, r = ok(c, "SELECT id FROM t ORDER BY id;")
    check("read own writes", good and r["rows"] == [[1], [3]], str(r))
    good, r = ok(c, "SELECT id, L2_DISTANCE(v,'[0,0]') AS d FROM t ORDER BY d LIMIT 2;")
    check("transaction vector search sees overlay", good and [row[0] for row in r["rows"]] == [1, 3], str(r))
    good, r = err(c, "INSERT INTO t VALUES (4,'c','[0,0]'),(3,'c','[0,0]');")
    check("failed statement reports duplicate", good, str(r))
    good, r = ok(c, "SELECT id FROM t ORDER BY id;")
    check("failed statement leaves prior transaction work intact", good and r["rows"] == [[1], [3]], str(r))
    ok(c, "ROLLBACK;")
    good, r = ok(c, "SELECT id, s FROM t ORDER BY id;")
    check("ROLLBACK restores rows", good and r["rows"] == [[1, "a"], [2, "a"]], str(r))
    ok(c, "BEGIN;")
    ok(c, "DELETE FROM t WHERE id = 1;", affected_rows=1)
    ok(c, "INSERT INTO t VALUES (1,'z','[9,9]');", affected_rows=1)
    ok(c, "COMMIT;")
    good, r = ok(c, "SELECT id, s FROM t ORDER BY id;")
    check("delete then reinsert same PK", good and r["rows"] == [[1, "z"], [2, "a"]], str(r))
    ok(c, "BEGIN;")
    ok(c, "INSERT INTO t VALUES (9,'q','[1,1]');", affected_rows=1)
    ok(c, "DELETE FROM t WHERE id = 9;", affected_rows=1)
    ok(c, "COMMIT;")
    good, r = ok(c, "SELECT id FROM t ORDER BY id;")
    check("insert then delete in one transaction is a no-op", good and r["rows"] == [[1], [2]], str(r))
    c.shutdown()


def section_recovery(binary, d):
    c = Client(binary, d)
    ok(c, "CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(16));")
    ok(c, "INSERT INTO t VALUES (1,'committed');")
    ok(c, "BEGIN;")
    ok(c, "INSERT INTO t VALUES (2,'uncommitted');")
    ok(c, "UPDATE t SET s = 'dirty' WHERE id = 1;")
    c.kill()
    c = Client(binary, d)
    good, r = ok(c, "SELECT id, s FROM t ORDER BY id;")
    check("SIGKILL: committed survives, uncommitted absent", good and r["rows"] == [[1, "committed"]], str(r))
    ok(c, "INSERT INTO t VALUES (3,'after');", affected_rows=1)
    c.shutdown()
    c = Client(binary, d)
    good, r = ok(c, "SELECT id FROM t ORDER BY id;")
    check("recovery remains usable after restart", good and r["rows"] == [[1], [3]], str(r))
    c.shutdown()


def main():
    with tempfile.TemporaryDirectory(prefix="mvd-formal-", ignore_cleanup_errors=True) as base:
        for index, section in enumerate(
                (section_schema, section_sql, section_vector, section_transactions, section_recovery)):
            directory = os.path.join(base, "case%d" % index)
            os.makedirs(directory)
            section(DEFAULT_BINARY, directory)
    print("ALL FORMAL COVERAGE CHECKS PASSED" if not FAILURES
          else "%d FAILURE(S)" % len(FAILURES))
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
