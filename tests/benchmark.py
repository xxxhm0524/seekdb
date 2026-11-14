"""Task-shaped benchmark with an independently evaluated float32 oracle."""
import argparse
import json
import math
from pathlib import Path
import sys
import time
import numpy as np
from client import Client, vector_bits

def run(directory, count=10000, dimensions=128, queries=1000, seed=194709):
    rng = np.random.default_rng(seed)
    vectors = rng.uniform(-1, 1, (count, dimensions)).astype(np.float32)
    # Include exact ties and float32 rounding, rather than only easy random data.
    vectors[1] = vectors[0]
    groups = rng.integers(0, 9, count)
    qvectors = rng.uniform(-1, 1, (queries, dimensions)).astype(np.float32)
    filters = [None if i % 2 == 0 else int(rng.integers(0, 11)) for i in range(queries)]
    db = Client(directory, timeout=60)
    try:
        db.execute(f"CREATE TABLE docs(id BIGINT PRIMARY KEY,category VARCHAR(16),content VARCHAR(128),embedding VECTOR({dimensions}));")
        start = time.perf_counter()
        for base in range(0, count, 100):
            values = []
            for key in range(base, min(base + 100, count)):
                values.append(f"({key},'group{groups[key]}','document {key}','{json.dumps(vectors[key].tolist(), separators=(',', ':'))}')")
            db.execute("BEGIN;")
            db.execute("INSERT INTO docs VALUES " + ",".join(values) + ";")
            db.execute("COMMIT;")
        write_seconds = time.perf_counter() - start
        start = time.perf_counter()
        db.execute("CREATE VECTOR INDEX embedding_idx ON docs(embedding);")
        query_sql = []
        for i, query in enumerate(qvectors):
            where = "" if filters[i] is None else f"WHERE category='group{filters[i]}'"
            query_sql.append(f"SELECT id,L2_DISTANCE(embedding,'{json.dumps(query.tolist(), separators=(',', ':'))}') AS distance FROM docs {where} ORDER BY distance LIMIT 10;")
        for sql in query_sql[:10]:
            db.execute(sql)
        prepare_seconds = time.perf_counter() - start
        start = time.perf_counter()
        answers = [db.execute(sql)["rows"] for sql in query_sql]
        query_seconds = time.perf_counter() - start
        # The oracle is outside the timed database window. Double accumulation
        # follows dimension order exactly; it does not call a vector search library.
        stored = vectors.astype(np.float64)
        recalls = {"unfiltered": [], "filtered": []}
        for i, query in enumerate(qvectors):
            squared = np.zeros(count, dtype=np.float64)
            for dimension in range(dimensions):
                diff = stored[:, dimension] - float(query[dimension])
                squared += diff * diff
            distances = np.sqrt(squared)
            ids = np.arange(count) if filters[i] is None else np.flatnonzero(groups == filters[i])
            wanted = sorted(ids.tolist(), key=lambda key: (float(distances[key]), key))[:10]
            actual = [row[0] for row in answers[i]]
            assert len(actual) == len(wanted) and len(set(actual)) == len(actual)
            assert actual == wanted, dict(query=i, actual=actual, expected=wanted)
            for key, d in answers[i]:
                assert math.isclose(d, float(distances[key]), abs_tol=1e-12, rel_tol=1e-12)
            if wanted:
                recalls["unfiltered" if filters[i] is None else "filtered"].append(len(set(wanted) & set(actual)) / len(wanted))
        db.restart(crash=True)
        assert db.execute("SELECT id FROM docs;")["rows"] == [[i] for i in range(count)]
        for key in rng.choice(count, min(count, 50), replace=False).tolist():
            row = db.execute(f"SELECT embedding,category,content FROM docs WHERE id={key};")["rows"][0]
            assert vector_bits(row[0])==vectors[key].tobytes()
            assert row[1:]==[f"group{groups[key]}",f"document {key}"]
        return dict(passed=True, rows=count, dimensions=dimensions, queries=queries, seed=seed,
                    write_seconds=write_seconds, query_prepare_seconds=prepare_seconds, query_seconds=query_seconds,
                    recall={name: sum(values)/len(values) for name, values in recalls.items()},
                    database_bytes=(Path(directory)/"database.mvd").stat().st_size,
                    numpy_version=np.__version__,
                    note="Local task-shaped measurement, not platform scores or official T/B/M accounting.")
    finally:
        db.kill()

if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True)
    parser.add_argument("--rows", type=int, default=10000)
    parser.add_argument("--dimensions", type=int, default=128)
    parser.add_argument("--queries", type=int, default=1000)
    parser.add_argument("--seed", type=int, default=194709)
    args = parser.parse_args()
    out = Path(args.output)
    out.mkdir(parents=True, exist_ok=False)
    result = run(out/"data", args.rows, args.dimensions, args.queries, args.seed)
    (out/"report.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result), flush=True)
