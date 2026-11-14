"""An allocation failure reading the newest root must not roll back old commits."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
from client import BINARY, Client, ROOT

def run(directory,shim):
    db=Client(directory)
    try:
        db.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,payload VARCHAR(32));")
        db.execute("INSERT INTO t VALUES"+",".join(f"({k},'committed-{k}')" for k in range(20))+";")
        db.kill()
        path=directory/"database.mvd";before=path.read_bytes()
        headers=[before[:4096],before[4096:8192]]
        latest=max(headers,key=lambda p:struct.unpack_from('<Q',p,8)[0])
        offset=struct.unpack_from('<Q',latest,16)[0]
        report=directory/"allocation-probe.json"
        env=dict(os.environ,LD_PRELOAD=str(shim),MVD_FAIL_CATALOG_OFFSET=str(offset),MVD_IO_REPORT=str(report))
        response=subprocess.run([BINARY,"--data-dir",str(directory)],input=b"",capture_output=True,env=env,timeout=15)
        probe=json.loads(report.read_text())
        assert probe["catalog_allocation_failures"]==1,"allocation fault was not injected"
        unchanged=path.read_bytes()==before
        assert unchanged,"allocation failure selected an older root and modified acknowledged data"
        assert response.returncode!=0 and b'"ready"' not in response.stdout,"allocation failure was silently ignored"
        db.start()
        assert db.execute("SELECT id,payload FROM t ORDER BY id;")["rows"]==[[k,f"committed-{k}"] for k in range(20)]
        return dict(passed=True,injected_allocation_failures=1,file_preserved=True,
                    file_sha256=hashlib.sha256(before).hexdigest(),acknowledged_rows_preserved=20)
    finally:db.kill()

if __name__=="__main__":
    assert os.name=="posix" and not os.environ.get("MVD_DOCKER_IMAGE")
    p=argparse.ArgumentParser();p.add_argument("--output",required=True);args=p.parse_args()
    out=Path(args.output).resolve();out.mkdir(parents=True,exist_ok=False)
    shim=out/"io-probe.so"
    subprocess.run(["cc","-shared","-fPIC","-O2",str(ROOT/"tests/io_probe.c"),"-o",str(shim),"-ldl"],check=True)
    try:result=run(out/"data",shim)
    except Exception as error:result=dict(passed=False,error=str(error))
    (out/"report.json").write_text(json.dumps(result,indent=2),encoding="utf-8")
    print(json.dumps(result),flush=True)
    raise SystemExit(0 if result["passed"] else 1)
