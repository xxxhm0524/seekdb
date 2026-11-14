"""Measure real pread bytes; unused wide text must not be read by TopK."""
import argparse
import json
import os
from pathlib import Path
import subprocess
from client import Client, ROOT

def run(directory, shim):
    assert os.name == "posix" and not os.environ.get("MVD_DOCKER_IMAGE")
    db = Client(directory, timeout=60, env={"MVD_VECTOR_CACHE_MIB":"0"})
    count, width = 5000, 32768
    try:
        db.execute(f"CREATE TABLE t(id BIGINT PRIMARY KEY,payload VARCHAR({width}),bucket BIGINT,v VECTOR(2));")
        payload = "x" * width
        for base in range(0,count,10):
            values = ",".join(f"({k},'{payload}',{k%7},'[{k%23},{k%29}]')" for k in range(base,base+10))
            db.execute("INSERT INTO t VALUES " + values + ";")
        db.shutdown()
        report = directory/"pread-bytes.json"
        db.env.update(LD_PRELOAD=str(shim), MVD_IO_REPORT=str(report))
        db.start()
        rows = db.execute("SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM t WHERE bucket=3 ORDER BY d LIMIT 10;")["rows"]
        wanted = sorted((k for k in range(count) if k%7 == 3), key=lambda k: ((k%23)**2+(k%29)**2,k))[:10]
        assert [r[0] for r in rows] == wanted
        db.shutdown()
        actual = json.loads(report.read_text())["database_read_bytes"]
        assert actual < 16*1024*1024, f"TopK read {actual} bytes, including unprojected wide text"
        return dict(passed=True, rows=count, live_text_bytes=count*width, database_read_bytes=actual)
    finally:
        db.kill()

if __name__ == "__main__":
    parser=argparse.ArgumentParser()
    parser.add_argument("--output", required=True)
    args=parser.parse_args()
    out=Path(args.output).resolve()
    out.mkdir(parents=True,exist_ok=False)
    shim=out/"io-probe.so"
    subprocess.run(["cc","-shared","-fPIC","-O2",str(ROOT/"tests/io_probe.c"),"-o",str(shim),"-ldl"],check=True)
    try:
        result=run(out/"data",shim)
    except Exception as error:
        result=dict(passed=False,error=str(error))
    (out/"report.json").write_text(json.dumps(result,indent=2),encoding="utf-8")
    print(json.dumps(result),flush=True)
    raise SystemExit(0 if result["passed"] else 1)
