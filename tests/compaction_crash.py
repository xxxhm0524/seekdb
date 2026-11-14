"""Kill the production ELF exactly before and after atomic file replacement."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time
from client import Client, ROOT

def run(directory, shim):
    assert os.name == "posix" and not os.environ.get("MVD_DOCKER_IMAGE")
    db = Client(directory, timeout=60)
    width, count = 32768, 400
    latest = "a" * width
    outcomes = []
    try:
        db.execute(f"CREATE TABLE t(id BIGINT PRIMARY KEY,payload VARCHAR({width}),v VECTOR(2));")
        for base in range(0, count, 20):
            values = ",".join(f"({k},'{latest}','[{k},0]')" for k in range(base,base+20))
            db.execute("INSERT INTO t VALUES " + values + ";")
        db.execute("CREATE VECTOR INDEX idx ON t(v);")
        for stage in ["before_replace", "after_replace"] * 2:
            while (directory/"database.mvd").stat().st_size <= 64*1024*1024:
                latest = ("b" if latest[0] != "b" else "c") * width
                db.execute(f"UPDATE t SET payload='{latest}';")
            db.shutdown()
            marker = directory/"compact-stop.marker"
            marker.unlink(missing_ok=True)
            db.env.update(LD_PRELOAD=str(shim), MVD_COMPACT_STOP=stage, MVD_COMPACT_MARKER=str(marker))
            db.start()
            outcome = {}
            def begin():
                try:
                    outcome["response"] = db.execute("BEGIN;")
                except Exception:
                    outcome["interrupted"] = True
            worker = threading.Thread(target=begin, daemon=True)
            worker.start()
            deadline = time.monotonic() + 20
            while not marker.exists() and time.monotonic() < deadline and db.process.poll() is None:
                time.sleep(0.002)
            assert marker.exists(), "production replacement syscall was not reached"
            assert marker.read_text() == stage
            db.kill()
            worker.join(timeout=10)
            assert not worker.is_alive()
            assert not outcome.get("response"), "database acknowledged before the injected stop"
            for key in ["LD_PRELOAD", "MVD_COMPACT_STOP", "MVD_COMPACT_MARKER"]:
                db.env.pop(key, None)
            db.start()
            assert db.execute("SELECT id FROM t;")["rows"] == [[k] for k in range(count)]
            assert db.execute("SELECT payload FROM t WHERE id=7;")["rows"] == [[latest]]
            assert db.execute("SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM t ORDER BY d LIMIT 3;")["rows"] == [[0,0.0],[1,1.0],[2,2.0]]
            assert not (directory/".compact-work/database.mvd").exists()
            # A new transaction must remain usable after either recovery path.
            db.execute("BEGIN;")
            db.execute("DELETE FROM t WHERE id=0;")
            db.execute("ROLLBACK;")
            assert db.execute("SELECT id FROM t WHERE id=0;")["rows"] == [[0]]
            outcomes.append(dict(stage=stage, stopped_before_response=True, recovery_passed=True))
        return dict(passed=True, cases=outcomes)
    finally:
        db.kill()

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    out = Path(args.output).resolve()
    out.mkdir(parents=True, exist_ok=False)
    shim = out/"io-probe.so"
    subprocess.run(["cc", "-shared", "-fPIC", "-O2", str(ROOT/"tests/io_probe.c"), "-o", str(shim), "-ldl"], check=True)
    result = run(out/"data", shim)
    (out/"report.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result), flush=True)
