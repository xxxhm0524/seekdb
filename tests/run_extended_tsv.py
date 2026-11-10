#!/usr/bin/env python3
"""Extended semantic checks over the JSONL protocol (errors, types, tx, restart)."""
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from protocol_client import Client, DEFAULT_BINARY  # noqa: E402

failures = []


def check(cond, message):
    print(("PASS " if cond else "FAIL ") + message)
    if not cond:
        failures.append(message)


def ok(client, sql, **fields):
    response = client.request(sql)
    good = response.get("ok") is True
    for key, value in fields.items():
        good = good and response.get(key) == value
    check(good, f"{sql} => {response}")
    return response


def error(client, sql, code):
    response = client.request(sql)
    good = (response.get("ok") is False and
            isinstance(response.get("error", {}).get("code"), str) and
            response["error"]["code"] == code)
    check(good, f"{sql} => {response}")
    return response


def main():
    binary = DEFAULT_BINARY
    with tempfile.TemporaryDirectory(prefix="mvd-ext-") as datadir:
        client = Client(binary, datadir)
        try:
            ok(client, "CREATE TABLE t (id BIGINT, s VARCHAR(2), v VECTOR(2), PRIMARY KEY (id))")
            ok(client, "INSERT INTO t VALUES (-9223372036854775808, 'a😀', '[1,2]'), (9223372036854775807, 'z', '[-1,2]')",
               affected_rows=2)
            error(client, "INSERT INTO t VALUES (3, 'abc', '[0,0]')", "varchar_too_long")
            error(client, "INSERT INTO t VALUES (3, 'x', '[1]')", "dimension_mismatch")
            error(client, "INSERT INTO t VALUES (3, 'x', '[NaN,0]')", "invalid_vector")
            error(client, "INSERT INTO t VALUES (3, 'x', '[1e100,0]')", "vector_overflow")
            response = ok(client, "SELECT id FROM t ORDER BY s")
            check(response.get("rows") == [[-9223372036854775808], [9223372036854775807]],
                  f"BIGINT extremes and order: {response}")
            ok(client, "BEGIN")
            ok(client, "UPDATE t SET s = 'q' WHERE id = -9223372036854775808", affected_rows=1)
            ok(client, "INSERT INTO t VALUES (5, 'n', '[2,1]')", affected_rows=1)
            response = ok(client, "SELECT id, s FROM t ORDER BY id")
            check(response.get("rows") == [[-9223372036854775808, "q"], [5, "n"], [9223372036854775807, "z"]],
                  f"transaction read-own-writes: {response}")
            ok(client, "ROLLBACK")
            response = ok(client, "SELECT id, s FROM t ORDER BY id")
            check(response.get("rows") == [[-9223372036854775808, "a😀"], [9223372036854775807, "z"]],
                  f"rollback restores: {response}")
            ok(client, "BEGIN")
            ok(client, "DELETE FROM t WHERE id = 9223372036854775807", affected_rows=1)
            ok(client, "INSERT INTO t VALUES (7, 'c', '[0,0]')", affected_rows=1)
            ok(client, "COMMIT")
            response = ok(client, "SELECT id FROM t ORDER BY id")
            check(response.get("rows") == [[-9223372036854775808], [7]], f"commit visible: {response}")
            response = ok(client, "SELECT v FROM t WHERE id = 7")
            check(response.get("rows") == [[[0.0, 0.0]]], f"vector projection is a JSON array: {response}")
            ok(client, "BEGIN")
            ok(client, "DELETE FROM t", affected_rows=2)
            client.shutdown()
            client = Client(binary, datadir)
            response = ok(client, "SELECT id, s FROM t ORDER BY id")
            check(response.get("rows") == [[-9223372036854775808, "a😀"], [7, "c"]],
                  f"shutdown rollback and restart: {response}")
            response = ok(client, "SELECT id, L2_DISTANCE(v, '[0,0]') AS d FROM t WHERE s = 'c'")
            check(response.get("rows") == [[7, 0.0]], f"distance: {response}")
        finally:
            try:
                client.shutdown()
            except Exception:
                client.kill()
    print("ALL PASS" if not failures else "FAILURES")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
