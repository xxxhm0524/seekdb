"""KD lower bounds vs dimension-ordered brute force on hostile float32 data."""
import argparse
import json
import math
from pathlib import Path
import sys
import numpy as np
from client import Client, vector_bits

def run(directory, cache="16"):
    rng = np.random.default_rng(849203)
    dimensions, n = 33, 257
    bits = rng.integers(0, 2**32, (n, dimensions), dtype=np.uint32)
    bits &= np.uint32(0xfeffffff)  # exponent 255 -> 253; all bit patterns finite
    vectors = bits.view(np.float32)
    vectors[0] = 0
    vectors[1] = vectors[0]
    vectors[2] = np.float32(np.finfo(np.float32).max)
    vectors[3] = -vectors[2]
    vectors[4] = np.float32(np.nextafter(np.float32(0), np.float32(1)))
    vectors[5,0] = np.float32(-0.0)
    queries = [np.zeros(dimensions, dtype=np.float32), vectors[2], vectors[3], vectors[4]]
    queries += [v.copy() for v in vectors[5:15]]
    keys = np.array([-(2**63), 2**63-1] + list(range(n-2)), dtype=np.int64)
    db = Client(directory, env={"MVD_VECTOR_CACHE_MIB": cache})
    checked = 0
    try:
        db.execute(f"CREATE TABLE t(id BIGINT PRIMARY KEY,bucket BIGINT,v VECTOR({dimensions}));")
        values = ",".join(f"({int(keys[i])},{i%4},'{json.dumps(vectors[i].tolist(), separators=(',', ':'))}')" for i in range(n))
        db.execute("INSERT INTO t VALUES " + values + ";")
        db.execute("CREATE VECTOR INDEX idx ON t(v);")
        for crash in [False, True]:
            if crash:
                db.restart(crash=True)
            projected=db.execute('SELECT id,v FROM t ORDER BY id;')['rows']
            permutation=sorted(range(n),key=lambda i:int(keys[i]))
            assert len(projected)==n
            for row,i in zip(projected,permutation):
                assert type(row[0]) is int and row[0]==int(keys[i])
                assert vector_bits(row[1])==vectors[i].tobytes(),(cache,i)
            for q in queries:
                d = np.zeros(n, dtype=np.float64)
                for dim in range(dimensions):
                    diff = vectors[:, dim].astype(np.float64) - float(q[dim])
                    d += diff * diff
                d = np.sqrt(d)
                for bucket in [None, 1, 9]:
                    ids = [i for i in range(n) if bucket is None or i % 4 == bucket]
                    ids.sort(key=lambda i: (float(d[i]), int(keys[i])))
                    where = "" if bucket is None else f"WHERE bucket={bucket}"
                    for k in [1, 10, n+5]:
                        for mode in ["exact", "ann"]:
                            rows = db.execute(f"SELECT id,L2_DISTANCE(v,'{json.dumps(q.tolist(), separators=(',', ':'))}') AS d FROM t {where} ORDER BY d LIMIT {k};", mode)["rows"]
                            assert [row[0] for row in rows] == [int(keys[i]) for i in ids[:k]], (cache, q.tolist(), bucket, k)
                            for j, (_, distance) in enumerate(rows):
                                assert math.isclose(distance, float(d[ids[j]]), rel_tol=1e-14, abs_tol=0)
                            checked += 1
        return dict(passed=True, queries=checked, rows=n, dimensions=dimensions, vector_cache_mib=cache)
    finally:
        db.kill()

if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    p = argparse.ArgumentParser()
    p.add_argument("--output", required=True)
    args = p.parse_args()
    out = Path(args.output)
    out.mkdir(parents=True, exist_ok=False)
    result = dict(passed=True, variants=[run(out/"indexed"), run(out/"disk", "0")])
    (out/"report.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result), flush=True)
