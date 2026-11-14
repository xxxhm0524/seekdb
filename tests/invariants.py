"""Independent state-machine, float32, crash and external-memory checks."""
import argparse
import copy
import json
import math
from pathlib import Path
import random
import struct
import sys
import time
from client import Client

def quote(s):
    return "'" + s.replace("\\", "\\\\").replace("'", "''") + "'"

def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]

def check_state(db, expected):
    actual = db.execute("SELECT * FROM docs ORDER BY id;")["rows"]
    wanted = [[key, *expected[key]] for key in sorted(expected)]
    assert actual == wanted, (actual[:5], wanted[:5])
    q = [0.125, -0.25, 2.0]
    for mode in ["exact", "ann"]:
        for bucket in [None, -1, 0, 2, 999]:
            keys = [k for k, row in expected.items() if bucket is None or row[0] == bucket]
            dist = {k: math.sqrt(sum((float(x) - float(y)) ** 2 for x, y in zip(expected[k][2], q))) for k in keys}
            keys.sort(key=lambda k: (dist[k], k))
            where = "" if bucket is None else f"WHERE bucket={bucket}"
            rows = db.execute(f"SELECT id,L2_DISTANCE(v,'{json.dumps(q)}') AS d FROM docs {where} ORDER BY d LIMIT 11;", mode)["rows"]
            assert [r[0] for r in rows] == keys[:11]
            for key, d in rows:
                assert math.isclose(d, dist[key], abs_tol=1e-12, rel_tol=1e-12), (key, d, dist[key])

def state_machine(directory, iterations=500):
    rng = random.Random(734871)
    db = Client(directory)
    try:
        db.execute("CREATE TABLE docs (id BIGINT PRIMARY KEY,bucket BIGINT,tag VARCHAR(20),v VECTOR(3));")
        committed, working = {}, {}
        transaction = False
        for step in range(iterations):
            op = rng.randrange(12)
            key = rng.randint(-150, 150)
            if op <= 3:
                row = [rng.randint(-1, 3), rng.choice(["中文😀", "a'b", "x\\y", "", "é", "𐀀"]), [f32(rng.uniform(-5, 5)) for _ in range(3)]]
                # Every fourth batch fails late, after the first row was staged.
                values = f"({key},{row[0]},{quote(row[1])},{quote(json.dumps(row[2]))})"
                duplicate = op == 3
                if duplicate:
                    values += "," + values
                response = db.execute("INSERT INTO docs VALUES " + values + ";", ok=None)
                assert response["ok"] == (key not in working and not duplicate)
                if response["ok"]:
                    working[key] = row
            elif op == 4:
                value = rng.randint(-3, 3)
                r = db.execute(f"UPDATE docs SET bucket={value} WHERE id>={key} AND id<{key+9};")
                affected = [k for k in working if key <= k < key + 9]
                assert r["affected_rows"] == len(affected)
                for k in affected:
                    working[k][0] = value
            elif op == 5:
                r = db.execute(f"DELETE FROM docs WHERE id>={key} AND id<{key+7};")
                keys = [k for k in working if key <= k < key+7]
                assert r["affected_rows"] == len(keys)
                for k in keys:
                    del working[k]
            elif op == 6:
                if transaction:
                    db.execute("COMMIT;")
                    committed = copy.deepcopy(working)
                    transaction = False
                else:
                    db.execute("BEGIN;")
                    transaction = True
            elif op == 7 and transaction:
                db.execute("ROLLBACK;")
                working = copy.deepcopy(committed)
                transaction = False
            elif op == 8:
                db.restart(crash=True)
                working = copy.deepcopy(committed)
                transaction = False
            elif op == 9:
                db.execute("CREATE VECTOR INDEX idx ON docs(v);", ok=None)
            elif op == 10:
                db.execute("UPDATE docs SET bucket=12,tag='this string is far too long for twenty characters' WHERE id>=-999;", ok=False)
            if not transaction:
                committed = copy.deepcopy(working)
            if step % 23 == 0:
                check_state(db, working)
        check_state(db, working)
        db.shutdown()
        db.start()
        check_state(db, committed)
        return {"operations": iterations, "rows": len(committed)}
    finally:
        db.kill()

def btree_depth(directory, count=60000):
    rng = random.Random(811947)
    keys = list(range(count))
    rng.shuffle(keys)
    db = Client(directory, timeout=60, env={"MVD_PAGE_CACHE_MIB": "0"})
    try:
        db.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,n BIGINT);")
        for start in range(0, count, 200):
            values = ",".join(f"({k},{k%13})" for k in keys[start:start+200])
            db.execute("INSERT INTO t VALUES " + values + ";")
        # 60k shuffled keys force leaf and internal node splits with eviction.
        for k in rng.sample(keys, min(200, count)):
            assert db.execute(f"SELECT * FROM t WHERE id={k};")["rows"] == [[k, k % 13]]
        db.execute("BEGIN;")
        db.execute("UPDATE t SET n=-9 WHERE id=0;")
        late_failure = ",".join(f"({count+10000+k},1)" for k in range(3000)) + ",(0,1)"
        db.execute("INSERT INTO t VALUES " + late_failure + ";", ok=False)
        assert db.execute("SELECT n FROM t WHERE id=0;")["rows"] == [[-9]]
        assert db.execute(f"SELECT id FROM t WHERE id>={count+10000};")["rows"] == []
        db.execute("COMMIT;")
        db.restart(crash=True)
        assert db.execute("SELECT n FROM t WHERE id=0;")["rows"] == [[-9]]
        db.execute("BEGIN;")
        db.execute("DELETE FROM t WHERE id>=0;")
        assert db.execute("SELECT id FROM t LIMIT 1;")["rows"] == []
        db.execute("ROLLBACK;")
        assert len(db.execute("SELECT id FROM t;")["rows"]) == count
        db.execute(f"DELETE FROM t WHERE id<{count//2};")
        db.restart(crash=True)
        got = db.execute("SELECT id FROM t;")["rows"]
        assert got == [[k] for k in range(count//2, count)]
        # Deleted separators and empty leaves remain valid for reinsertion.
        for start in range(0, count//2, 200):
            values = ",".join(f"({k},-1)" for k in range(start, min(start+200, count//2)))
            db.execute("INSERT INTO t VALUES " + values + ";")
        assert db.execute("SELECT id FROM t ORDER BY id DESC LIMIT 10;")["rows"] == [[k] for k in range(count-1, count-11, -1)]
        db.restart()
        assert len(db.execute("SELECT id FROM t;")["rows"]) == count
        return {"rows": count, "database_bytes": (Path(directory)/"database.mvd").stat().st_size}
    finally:
        db.kill()

def value_boundaries(directory):
    db = Client(directory)
    try:
        db.execute("CREATE TABLE unicode_table (n BIGINT, id BIGINT PRIMARY KEY, s VARCHAR(3), v VECTOR(2));")
        db.execute("INSERT INTO unicode_table (s,id,v,n) VALUES ('中😀é',-9223372036854775808,'[16777217,1.401298464324817e-45]',9223372036854775807);")
        r = db.execute("SELECT * FROM unicode_table;")["rows"]
        assert r == [[9223372036854775807, -9223372036854775808, "中😀é", [16777216.0, f32(1.401298464324817e-45)]]], r
        for vector in ["[NaN,0]", "[Infinity,0]", "[1e400,0]", "[1e39,0]", "[1]", "[1,2,3]", "[true,0]", "[null,0]", "['1',0]", "{\"0\":0}"]:
            db.execute("INSERT INTO unicode_table VALUES(0,1,'x'," + quote(vector) + ");", ok=False)
        for vector in ["[1 2,0]", "[1,'2']", '[1,"2"]', "[1 e2,0]", "[1,]", "[,1]", "[[1],0]", "[true,0]"]:
            db.execute("INSERT INTO unicode_table VALUES(0,1,'x'," + vector + ");", ok=False)
        for bad in ["9223372036854775808", "-9223372036854775809", "1.5", "1e2", "'1'"]:
            db.execute(f"INSERT INTO unicode_table VALUES (0,{bad},'x','[0,0]');", ok=False)
        db.execute("INSERT INTO unicode_table VALUES (0,2,'中😀éa','[0,0]');", ok=False)
        # This decimal is above FLT_MAX as a double, but rounds to finite
        # FLT_MAX as float32. Only conversion overflow should be rejected.
        db.execute("INSERT INTO unicode_table VALUES (0,3,'max','[3.4028235e38,-3.4028235e38]');")
        assert db.execute("SELECT v FROM unicode_table WHERE id=3;")["rows"] == [[
            [f32(3.4028235e38), f32(-3.4028235e38)]]]
        db.execute("DELETE FROM unicode_table WHERE id=3;")
        db.execute("SELECT missing FROM unicode_table LIMIT 0;", ok=False)
        db.execute("SELECT id FROM unicode_table WHERE missing=1;", ok=False)
        db.execute("SELECT id FROM unicode_table ORDER BY missing LIMIT 0;", ok=False)
        for sql in ["ALTER TABLE unicode_table ADD x BIGINT;", "SELECT COUNT(*) FROM unicode_table;", "BEGIN; BEGIN;", "UPDATE unicode_table SET id=12;", "DELETE FROM unicode_table WHERE n=0 OR n=1;"]:
            db.execute(sql, ok=False)
        assert db.execute("SELECT * FROM unicode_table;")["rows"] == r
        distance = db.execute("SELECT L2_DISTANCE(v,'[0,0]') FROM unicode_table;")
        assert distance["columns"] == ["l2_distance"]
        for request_id in [9007199254740993, -9223372036854775808, "test", None, {"nested": [1, 2]}]:
            response = db.send(dict(id=request_id, op="execute", sql="SELECT id FROM unicode_table;"))
            assert response["ok"]
        db.execute("CREATE TABLE `select` (`l2_distance` BIGINT PRIMARY KEY,`from` VARCHAR(3));")
        db.execute("INSERT INTO `select` VALUES(1,'中😀');")
        assert db.execute("SELECT l2_distance,`from` FROM `select`;")["rows"] == [[1,"中😀"]]
        db.restart(crash=True)
        assert db.execute("SELECT * FROM unicode_table;")["rows"] == r
        return {"invalid_values_rejected": 21, "unicode_and_bigint": True}
    finally:
        db.kill()

def external_sort(directory, count=9000):
    rng = random.Random(1693)
    db = Client(directory, timeout=60)
    try:
        db.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(2000),bucket BIGINT);")
        records = [(k, rng.choice(["中", "😀", "a", "é"]) + "x"*900 + str(rng.randrange(300)), rng.randrange(5)) for k in range(count)]
        for start in range(0, count, 100):
            values = ",".join(f"({k},{quote(s)},{b})" for k, s, b in records[start:start+100])
            db.execute("INSERT INTO t VALUES " + values + ";")
        expected = sorted(records, key=lambda r: (r[1], -r[2], r[0]))
        got = db.execute("SELECT id FROM t ORDER BY s,bucket DESC;")["rows"]
        assert got == [[r[0]] for r in expected]
        assert db.execute("SELECT id FROM t ORDER BY s,bucket DESC LIMIT 1500;")["rows"] == [[r[0]] for r in expected[:1500]]
        # Force response spill, then verify subsequent protocol framing.
        rows = db.execute("SELECT s FROM t;")["rows"]
        assert rows == [[r[1]] for r in records]
        assert db.execute("SELECT id FROM t LIMIT 1;")["rows"] == [[0]]
        assert not list(Path(directory).glob("sort-*.tmp"))
        assert not (Path(directory)/"response.tmp").exists()
        return {"sorted_rows": count, "response_bytes": sum(len(r[1].encode()) for r in records)}
    finally:
        db.kill()

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True)
    parser.add_argument("--quick", action="store_true")
    args = parser.parse_args()
    root = Path(args.output)
    root.mkdir(parents=True, exist_ok=False)
    tests = [("value_boundaries", value_boundaries), ("state_machine", state_machine),
             ("btree_depth", lambda p: btree_depth(p, 3000 if args.quick else 60000)),
             ("external_sort", lambda p: external_sort(p, 1000 if args.quick else 9000))]
    reports = []
    for name, test in tests:
        start = time.monotonic()
        try:
            details = test(root / name)
            report = dict(test=name, passed=True, details=details)
        except Exception as e:
            import traceback
            report = dict(test=name, passed=False, error=str(e), traceback=traceback.format_exc())
        report["seconds"] = time.monotonic() - start
        reports.append(report)
        print(json.dumps(report, ensure_ascii=False), flush=True)
        (root/"report.json").write_text(json.dumps(dict(passed=all(x["passed"] for x in reports), tests=reports), ensure_ascii=False, indent=2), encoding="utf-8")
    return 0 if all(r["passed"] for r in reports) else 1

if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.exit(main())
