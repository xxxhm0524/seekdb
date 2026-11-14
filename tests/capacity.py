"""Actually exceed the container memory budget with persistent user data."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time
from client import Client

def run(directory, rows=5000):
    assert os.environ.get("MVD_DOCKER_IMAGE"), "This test requires a real memory-limited Docker container"
    memory = 96 * 1024 * 1024
    db = Client(directory, timeout=60, env={"MVD_DOCKER_MEMORY": "96m", "MVD_VECTOR_CACHE_MIB": "0", "MVD_PAGE_CACHE_MIB": "1"})
    payload = "x" * 32768
    try:
        db.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,content VARCHAR(32768),bucket BIGINT,v VECTOR(2));")
        start = time.monotonic()
        for base in range(0, rows, 10):
            values = ",".join(f"({key},'{payload}',{key%7},'[{key%23},{key%29}]')" for key in range(base, min(base+10, rows)))
            db.execute("INSERT INTO t VALUES " + values + ";")
        data_bytes = (Path(directory)/"database.mvd").stat().st_size
        assert rows * len(payload) > memory and data_bytes > memory
        peak = int(subprocess.check_output(["docker", "exec", db.container, "cat", "/sys/fs/cgroup/memory.peak"]))
        db.execute("CREATE VECTOR INDEX idx ON t(v);")
        for mode in ["exact", "ann"]:
            expected = sorted((key for key in range(rows) if key % 7 == 3), key=lambda k: ((k%23)**2+(k%29)**2, k))[:10]
            actual = db.execute("SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM t WHERE bucket=3 ORDER BY d LIMIT 10;", mode)["rows"]
            assert [row[0] for row in actual] == expected
        db.execute("BEGIN;")
        db.execute("UPDATE t SET bucket=-1 WHERE id<100;")
        db.execute("DELETE FROM t WHERE id>=100 AND id<200;")
        db.restart(crash=True)
        assert len(db.execute("SELECT id FROM t;")["rows"]) == rows
        assert db.execute("SELECT bucket FROM t WHERE id=0;")["rows"] == [[0]]
        db.execute("BEGIN;")
        db.execute("UPDATE t SET bucket=-1 WHERE id<100;")
        db.execute("DELETE FROM t WHERE id>=100 AND id<200;")
        db.execute("COMMIT;")
        db.restart(crash=True)
        assert len(db.execute("SELECT id FROM t;")["rows"]) == rows - 100
        assert db.execute("SELECT bucket FROM t WHERE id=0;")["rows"] == [[-1]]
        assert db.execute(f"SELECT content FROM t WHERE id={rows-1};")["rows"] == [[payload]]
        return dict(passed=True, rows=rows, logical_payload_bytes=rows*len(payload), database_bytes=data_bytes,
                    memory_limit_bytes=memory, container_memory_peak_bytes=peak, seconds=time.monotonic()-start)
    finally:
        db.kill()

if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    p = argparse.ArgumentParser()
    p.add_argument("--output", required=True)
    args = p.parse_args()
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=False)
    result = run(output/"data")
    (output/"report.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result), flush=True)
