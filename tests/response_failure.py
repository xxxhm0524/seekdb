"""ENOSPC at a response's final flush must not publish a partial JSON prefix."""
import argparse
import json
import os
from pathlib import Path
import subprocess
from client import Client, ROOT

def run(directory, shim):
    assert os.name == "posix" and not os.environ.get("MVD_DOCKER_IMAGE")
    db = Client(directory)
    try:
        db.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,payload VARCHAR(32768));")
        values=",".join(f"({k},'{'x'*32768}')" for k in range(10)) + ",(10,'tail')"
        db.execute("INSERT INTO t VALUES " + values + ";")
        db.shutdown()
        report=directory/"io-probe.json"
        db.env.update(LD_PRELOAD=str(shim),MVD_FAIL_RESPONSE_FLUSH="1",MVD_IO_REPORT=str(report))
        db.start()
        response=None
        protocol_error=None
        try:
            response=db.send(dict(id={'large':10**400},op='execute',sql="SELECT payload FROM t ORDER BY id;",search_mode='exact'))
        except Exception as error:
            protocol_error=str(error)
        db.process.wait(timeout=10)
        probe=json.loads(report.read_text())
        assert probe["response_flush_failures"]>0, "ENOSPC injector did not reach the final response flush"
        assert response is not None and response.get("ok") is False, f"partial response instead of one JSON error: {protocol_error}"
        assert response["error"]["code"]=="io_error"
        db.kill()
        for key in ["LD_PRELOAD","MVD_FAIL_RESPONSE_FLUSH","MVD_IO_REPORT"]:
            db.env.pop(key,None)
        db.start()
        assert db.execute("SELECT id FROM t;")["rows"]==[[k] for k in range(11)]
        return dict(passed=True,injected_failures=probe["response_flush_failures"],complete_error_response=True,large_composite_id_preserved=True,persistent_rows_preserved=11)
    finally:
        db.kill()

if __name__=="__main__":
    p=argparse.ArgumentParser()
    p.add_argument("--output",required=True)
    args=p.parse_args()
    out=Path(args.output).resolve();out.mkdir(parents=True,exist_ok=False)
    shim=out/"io-probe.so"
    subprocess.run(["cc","-shared","-fPIC","-O2",str(ROOT/"tests/io_probe.c"),"-o",str(shim),"-ldl"],check=True)
    try:
        result=run(out/"data",shim)
    except Exception as error:
        result=dict(passed=False,error=str(error))
    (out/"report.json").write_text(json.dumps(result,indent=2),encoding="utf-8")
    print(json.dumps(result),flush=True)
    raise SystemExit(0 if result["passed"] else 1)
