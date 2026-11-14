"""Kill repeatedly inside recovery's actual uncommitted-tail truncation."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time
from client import BINARY, Client, ROOT

def run(directory,shim):
    db=Client(directory)
    observations=[]
    try:
        db.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,payload VARCHAR(1024),v VECTOR(2));")
        values=",".join(f"({k},'committed-{k}','[{k},0]')" for k in range(20))
        db.execute("INSERT INTO t VALUES "+values+";")
        durable_size=(directory/"database.mvd").stat().st_size
        db.execute("BEGIN;")
        db.execute("UPDATE t SET payload='pending',v='[99,99]' WHERE id=1;")
        db.execute("DELETE FROM t WHERE id=2;")
        db.execute("INSERT INTO t VALUES(99,'"+"x"*1024+"','[0,0]');")
        db.kill()
        assert (directory/"database.mvd").stat().st_size>durable_size
        for stage in ["before_truncate","before_truncate","after_truncate","after_truncate"]:
            marker=directory/"recovery-stop.marker";marker.unlink(missing_ok=True)
            environment=dict(os.environ,LD_PRELOAD=str(shim),MVD_RECOVERY_STOP=stage,MVD_RECOVERY_MARKER=str(marker))
            stderr=open(directory/"recovery-probe.stderr.log","ab")
            process=subprocess.Popen([BINARY,"--data-dir",str(directory)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=stderr,env=environment)
            deadline=time.monotonic()+10
            try:
                while not marker.exists() and process.poll() is None and time.monotonic()<deadline:time.sleep(0.002)
                assert marker.exists(),"production recovery truncation was not reached"
                assert marker.read_text()==stage
                if stage=="after_truncate":assert (directory/"database.mvd").stat().st_size==durable_size
            finally:
                process.kill();stdout,_=process.communicate(timeout=10);stderr.close()
            assert b'"ready"' not in stdout,"stop occurred after recovery readiness"
            observations.append(dict(stage=stage,interrupted_before_ready=True))
        db.start()
        assert db.execute("SELECT id FROM t ORDER BY id;")["rows"]==[[k] for k in range(20)]
        assert db.execute("SELECT payload,v FROM t WHERE id=1;")["rows"]==[["committed-1",[1.0,0.0]]]
        assert db.execute("SELECT id FROM t WHERE id=2;")["rows"]==[[2]]
        assert db.execute("SELECT id FROM t WHERE id=99;")["rows"]==[]
        assert db.execute("SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM t ORDER BY d LIMIT 3;")["rows"]==[[0,0.0],[1,1.0],[2,2.0]]
        return dict(passed=True,consecutive_recovery_interruptions=observations,committed_rows_preserved=20,uncommitted_changes_rolled_back=True)
    finally:
        db.kill()

if __name__=="__main__":
    assert os.name=="posix" and not os.environ.get("MVD_DOCKER_IMAGE")
    parser=argparse.ArgumentParser();parser.add_argument("--output",required=True);args=parser.parse_args()
    out=Path(args.output).resolve();out.mkdir(parents=True,exist_ok=False)
    shim=out/"io-probe.so"
    subprocess.run(["cc","-shared","-fPIC","-O2",str(ROOT/"tests/io_probe.c"),"-o",str(shim),"-ldl"],check=True)
    try:result=run(out/"data",shim)
    except Exception as error:result=dict(passed=False,error=str(error))
    (out/"report.json").write_text(json.dumps(result,indent=2),encoding="utf-8")
    print(json.dumps(result),flush=True)
    raise SystemExit(0 if result["passed"] else 1)
