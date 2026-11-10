#!/usr/bin/env python3
"""Small performance benchmark matching the published round2 workload:
10,000 rows x 128 dims, 1,000 TopK queries (K=10), half with a category filter.
Also verifies every returned id against a NumPy brute-force truth."""
import argparse
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from protocol_client import Client, DEFAULT_BINARY  # noqa: E402

BINARY = DEFAULT_BINARY


def make_client(binary, datadir, threads=2):
    return Client(binary, datadir, extra=["--query-threads=%d" % threads])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=BINARY)
    ap.add_argument("--rows", type=int, default=10000)
    ap.add_argument("--dim", type=int, default=128)
    ap.add_argument("--queries", type=int, default=1000)
    ap.add_argument("--topk", type=int, default=10)
    ap.add_argument("--seed", type=int, default=20261109)
    args = ap.parse_args()

    rng = np.random.default_rng(args.seed)
    vectors = rng.random((args.rows, args.dim), dtype=np.float32)
    cats = np.array(["c%d" % (i % 10) for i in range(args.rows)])
    ids = np.arange(1, args.rows + 1, dtype=np.int64)

    base = tempfile.mkdtemp(prefix="mvdbench_")
    data = os.path.join(base, "data")
    os.makedirs(data)
    c = make_client(args.binary, data)
    print(c.request("CREATE TABLE items (id BIGINT PRIMARY KEY, cat VARCHAR(8), v VECTOR(%d));" % args.dim))

    t0 = time.perf_counter()
    batch = 100
    for start in range(0, args.rows, batch):
        parts = []
        for i in range(start, min(start + batch, args.rows)):
            lit = ",".join("%.8g" % x for x in vectors[i])
            parts.append("(%d,'%s','[%s]')" % (ids[i], cats[i], lit))
        r = c.request("INSERT INTO items VALUES " + ",".join(parts) + ";")
        if r.get("affected_rows") != len(parts):
            raise RuntimeError("insert batch failed: %s" % r)
    insert_time = time.perf_counter() - t0
    print("insert %d rows: %.3f s (%.2f rows/s)" % (args.rows, insert_time, args.rows / insert_time))

    vrng = np.random.default_rng(args.seed + 1)
    query_vecs = vrng.random((args.queries, args.dim), dtype=np.float32)
    use_filter = np.array([True] * (args.queries // 2) + [False] * (args.queries - args.queries // 2))
    filter_cats = np.array(["c%d" % (i % 10) for i in range(args.queries)])

    results = []
    t0 = time.perf_counter()
    for qi in range(args.queries):
        q = query_vecs[qi]
        cat = filter_cats[qi] if use_filter[qi] else None
        lit = ",".join("%.8g" % x for x in q)
        if cat is None:
            sql = "SELECT id, L2_DISTANCE(v,'[%s]') AS d FROM items ORDER BY d LIMIT %d;" % (lit, args.topk)
        else:
            sql = ("SELECT id, L2_DISTANCE(v,'[%s]') AS d FROM items WHERE cat='%s' "
                   "ORDER BY d LIMIT %d;" % (lit, cat, args.topk))
        r = c.request(sql)
        if "error" in r:
            raise RuntimeError("query failed: %s" % r)
        results.append([row[0] for row in r["rows"]])
    query_time = time.perf_counter() - t0
    failures = 0
    for qi, got in enumerate(results):
        q = query_vecs[qi]
        cat = filter_cats[qi] if use_filter[qi] else None
        mask = np.ones(args.rows, dtype=bool) if cat is None else (cats == cat)
        idx = np.flatnonzero(mask)
        diff = vectors[idx] - q
        dist2 = np.einsum("ij,ij->i", diff, diff)
        order = np.lexsort((ids[idx], dist2))[:args.topk]
        truth = ids[idx][order].tolist()
        if got != truth:
            failures += 1
            if failures <= 3:
                print("query %d mismatch: got %s truth %s" % (qi, got, truth))
    print("queries %d (K=%d, half filtered): %.3f s (%.2f qps, %.3f ms/query)"
          % (args.queries, args.topk, query_time, args.queries / query_time, query_time * 1000 / args.queries))
    print("recall check: %s" % ("PASS" if failures == 0 else "FAIL (%d)" % failures))
    c.shutdown()
    shutil.rmtree(base, ignore_errors=True)
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
