"""Interrupt real commits and startup; no in-engine crash simulation."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time
from client import Client, BINARY

def run(directory):
    assert not os.environ.get("MVD_DOCKER_IMAGE"), "Use the extracted production ELF for precise native kill timing"
    db = Client(directory, timeout=30)
    committed, keys = 0, {0}
    observations = []
    try:
        db.execute("CREATE TABLE a(id BIGINT PRIMARY KEY,generation BIGINT,payload VARCHAR(1024),v VECTOR(2));")
        db.execute("CREATE TABLE b(id BIGINT PRIMARY KEY,generation BIGINT);")
        db.execute("INSERT INTO a VALUES(0,0,'initial','[0,0]');")
        db.execute("INSERT INTO b VALUES(0,0);")
        for sequence, delay in enumerate([0, 0.0001, 0.0005, 0.001, 0.003, 0.01, 0.03] * 2, 1):
            db.execute("BEGIN;")
            db.execute(f"UPDATE a SET generation={sequence},v='[{sequence},0]' WHERE id=0;")
            db.execute(f"UPDATE b SET generation={sequence} WHERE id=0;")
            added = set(range(sequence*1000, sequence*1000+500))
            values = ",".join(f"({key},{sequence},'{'x'*1024}','[0,0]')" for key in sorted(added))
            db.execute("INSERT INTO a VALUES " + values + ";")
            outcome = {}
            started = threading.Event()
            def commit():
                started.set()
                try:
                    outcome["response"] = db.execute("COMMIT;")
                except Exception:
                    outcome["interrupted"] = True
            thread = threading.Thread(target=commit, daemon=True)
            thread.start()
            assert started.wait(2)
            time.sleep(delay)
            db.kill()
            thread.join(timeout=10)
            assert not thread.is_alive()
            db.start()
            generation_a = db.execute("SELECT generation FROM a WHERE id=0;")["rows"][0][0]
            generation_b = db.execute("SELECT generation FROM b WHERE id=0;")["rows"][0][0]
            assert generation_a == generation_b and generation_a in [committed, sequence]
            if outcome.get("response", {}).get("ok"):
                assert generation_a == sequence, "acknowledged commit was lost"
            if generation_a == sequence:
                keys.update(added)
                committed = sequence
            assert db.execute("SELECT id FROM a;")["rows"] == [[key] for key in sorted(keys)]
            assert db.execute("SELECT v FROM a WHERE id=0;")["rows"] == [[[float(committed), 0.0]]]
            observations.append(dict(delay_seconds=delay, acknowledged=bool(outcome.get("response", {}).get("ok")), recovered_generation=committed))
        db.shutdown()
        killed_before_ready = 0
        for delay in [0, 0.0001, 0.0005, 0.001, 0.003] * 3:
            p = subprocess.Popen([BINARY, "--data-dir", str(directory)], stdin=subprocess.PIPE,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            time.sleep(delay)
            p.kill()
            stdout, _ = p.communicate(timeout=10)
            killed_before_ready += b'"ready"' not in stdout
        db.start()
        assert db.execute("SELECT generation FROM a WHERE id=0;")["rows"] == [[committed]]
        assert db.execute("SELECT id FROM a;")["rows"] == [[key] for key in sorted(keys)]
        return dict(passed=True, commit_interruptions=observations, startup_interruptions=15,
                    startup_kills_before_ready=killed_before_ready, committed_rows=len(keys))
    finally:
        db.kill()

if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    out = Path(args.output)
    out.mkdir(parents=True, exist_ok=False)
    result = run(out/"data")
    (out/"report.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result), flush=True)
