"""Fixed live data must not leave an unbounded history of copied rows/pages."""
import argparse
import json
from pathlib import Path
import sys
from client import Client

def run(directory):
    db = Client(directory, timeout=60)
    count, width = 400, 32768
    observed = []
    try:
        db.execute(f"CREATE TABLE t(id BIGINT PRIMARY KEY,payload VARCHAR({width}),v VECTOR(2));")
        initial = "a" * width
        for base in range(0, count, 20):
            values = ",".join(f"({key},'{initial}','[{key},0]')" for key in range(base, base+20))
            db.execute("INSERT INTO t VALUES " + values + ";")
        for generation in range(16):
            payload = chr(98 + generation) * width
            result = db.execute(f"UPDATE t SET payload='{payload}';")
            assert result["affected_rows"] == count
            size = (Path(directory)/"database.mvd").stat().st_size
            observed.append(size)
            assert size <= 80 * 1024 * 1024, f"fixed 12.5 MiB payload grew to {size} bytes after {generation+1} updates"
            assert db.execute("SELECT payload FROM t WHERE id=7;")["rows"] == [[payload]]
            if generation % 3 == 0:
                db.restart(crash=True)
                assert db.execute("SELECT payload FROM t WHERE id=7;")["rows"] == [[payload]]
        return dict(passed=True, live_payload_bytes=count*width, maximum_database_bytes=max(observed), sizes=observed)
    finally:
        db.kill()

if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    out = Path(args.output)
    out.mkdir(parents=True, exist_ok=False)
    try:
        result = run(out/"data")
    except Exception as error:
        result = dict(passed=False, error=str(error))
    (out/"report.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result), flush=True)
    sys.exit(0 if result["passed"] else 1)
